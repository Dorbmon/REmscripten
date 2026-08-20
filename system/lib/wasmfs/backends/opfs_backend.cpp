// Copyright 2022 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <emscripten/threading.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <errno.h>
#include <memory>
#include <mutex>
#include <stdlib.h>

#include <string>
#include <utility>
#include <vector>

#include "backend.h"
#include "file.h"
#include "opfs_backend.h"
#include "support.h"
#include "thread_utils.h"
#include "wasmfs.h"

using namespace wasmfs;

namespace {

using ProxyWorker = emscripten::ProxyWorker;
using ProxyingQueue = emscripten::ProxyingQueue;

#ifdef WASMFS_OPFS_PROFILE_DRAIN_TEST
extern "C" void wasmfs_opfs_profile_drain_test_maybe_block_file_operation();
extern "C" void wasmfs_opfs_profile_drain_test_note_sealing();
#endif

constexpr size_t kMaxProfileLeaseNameLength = 128;
// HandleAllocator reserves ID 0. The OPFS root is initialized in slot 1 and
// remains there for the lifetime of its backend's ProxyWorker.
constexpr int kOPFSRootDirectoryID = 1;

bool IsValidProfileLeaseName(const char* name) {
  if (!name) {
    return false;
  }

  for (size_t i = 0; i <= kMaxProfileLeaseNameLength; ++i) {
    unsigned char c = name[i];
    if (c == '\0') {
      return i != 0;
    }
    if (i == kMaxProfileLeaseNameLength) {
      return false;
    }
    if (!(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
          ('0' <= c && c <= '9') || c == '.' || c == '-' || c == '_')) {
      return false;
    }
  }

  WASMFS_UNREACHABLE("Profile lease name validation should always return");
}

class Worker {
public:
#ifdef __EMSCRIPTEN_PTHREADS__
  ProxyWorker proxy;

  template<typename T> void operator()(T func) { proxy(func); }
#else
  // When used with JSPI on the main thread the various wasmfs_opfs_* functions
  // can be directly executed since they are all async.
  template<typename T> void operator()(T func) {
    if constexpr (std::is_invocable_v<T&, ProxyingQueue::ProxyingCtx>) {
      // TODO: Find a way to remove this, since it's unused.
      ProxyingQueue::ProxyingCtx p;
      func(p);
    } else {
      func();
    }
  }
#endif
};

// File wrappers can outlive the particular OpenFileState that observed a
// rejected SyncAccessHandle close. Keep this per-backend state separately so a
// later terminal drain cannot mistake an already-removed descriptor table for
// proof that browser-side access handles are no longer live.
class TerminalCloseState {
  std::atomic<int> firstError = 0;

public:
  void recordFailedAccessClose(int error) {
    if (error >= 0) {
      error = -EIO;
    }
    int expected = 0;
    (void)firstError.compare_exchange_strong(expected, error);
  }

  int getFailedAccessCloseError() const { return firstError.load(); }
};

// The cooperative profile lease is an explicit admission domain, separate
// from ordinary OPFS. Public WasmFS operations hold an external token from
// discovery of a leased file/directory until their outer syscall returns. A
// scoped drain flips Active to Sealing before waiting for those tokens. File
// wrappers share this state as a second line of defense for direct backend
// work; only the drain thread may use their internal operations while sealed.
class ProfileLeaseState {
public:
  enum class State { Unleased, Active, Sealing, Released, Failed };

  class InternalOperation {
    ProfileLeaseState& state;
    bool tracked = false;
    int error = 0;

  public:
    explicit InternalOperation(ProfileLeaseState& state) : state(state) {
      std::unique_lock<std::mutex> lock(state.mutex);
      if (state.state == State::Unleased) {
        return;
      }
      if (state.state == State::Active) {
        ++state.activeOperations;
        tracked = true;
        return;
      }
      // An outer public operation that acquired an external token before the
      // state changed to Sealing is already admitted. It must be able to
      // reach its later virtual-file operation while beginDrain() waits for
      // that token to be released. New operations have no such TLS ownership
      // and remain rejected; the drain thread is the other narrow exception.
      if (state.state == State::Sealing &&
          (drainState == &state || state.currentThreadOwnsExternalOperation())) {
        return;
      }
      error = -ESHUTDOWN;
    }

    InternalOperation(const InternalOperation&) = delete;
    InternalOperation& operator=(const InternalOperation&) = delete;

    ~InternalOperation() {
      if (tracked) {
        state.releaseOperation();
      }
    }

    explicit operator bool() const { return error == 0; }
    int getError() const { return error; }
  };

private:
  mutable std::mutex mutex;
  std::condition_variable operationCV;
  State state = State::Unleased;
  size_t activeOperations = 0;

  static thread_local ProfileLeaseState* drainState;
  // One outer WasmFS operation holds at most one deduplicated token for a
  // backend, but use a vector rather than a boolean to remain correct if an
  // internal user holds nested tokens on one thread. This TLS ownership is
  // deliberately not a general reentrancy grant: it only keeps work admitted
  // before the Sealing transition alive until its outer operation returns.
  static thread_local std::vector<ProfileLeaseState*> externalStates;

  bool currentThreadOwnsExternalOperation() const {
    return std::find(externalStates.begin(), externalStates.end(), this) !=
           externalStates.end();
  }

  void releaseOperation() {
    std::unique_lock<std::mutex> lock(mutex);
    assert(activeOperations > 0);
    --activeOperations;
    if (activeOperations == 0) {
      operationCV.notify_all();
    }
  }

public:
  void acquiredLease() {
    std::unique_lock<std::mutex> lock(mutex);
    assert(state == State::Unleased);
    state = State::Active;
  }

  int acquireExternalOperation() {
    std::unique_lock<std::mutex> lock(mutex);
    if (state == State::Unleased) {
      return 0;
    }
    if (state != State::Active) {
      return -ESHUTDOWN;
    }
    ++activeOperations;
    externalStates.push_back(this);
    return 0;
  }

  void releaseExternalOperation() {
    std::unique_lock<std::mutex> lock(mutex);
    // Unleased OPFS uses the Backend default no-op admission semantics. The
    // generic outer WasmFS operation still records the backend pointer, so
    // its later release must remain a no-op in this state.
    if (state == State::Unleased) {
      return;
    }
    auto external =
      std::find(externalStates.begin(), externalStates.end(), this);
    assert(external != externalStates.end());
    externalStates.erase(external);
    assert(activeOperations > 0);
    --activeOperations;
    if (activeOperations == 0) {
      operationCV.notify_all();
    }
  }

  bool isLeasedProfileBackend() const {
    std::unique_lock<std::mutex> lock(mutex);
    return state != State::Unleased;
  }

  bool supportsRecordLocks() const {
    std::unique_lock<std::mutex> lock(mutex);
    // A fcntl operation which acquired an external token before sealing must
    // finish under the same continuity rule as its eventual OPFS virtual
    // call. New fcntl operations cannot reach this check after sealing:
    // syscall admission rejects them before record-lock inspection.
    return state == State::Active ||
           (state == State::Sealing && currentThreadOwnsExternalOperation());
  }

  bool hasLiveLease() const {
    std::unique_lock<std::mutex> lock(mutex);
    return state == State::Active || state == State::Sealing ||
           state == State::Failed;
  }

  bool hasFailedDrain() const {
    std::unique_lock<std::mutex> lock(mutex);
    return state == State::Failed;
  }

  int beginDrain() {
    std::unique_lock<std::mutex> lock(mutex);
    if (state == State::Unleased) {
      return -ENOTSUP;
    }
    if (state == State::Sealing) {
      return -EBUSY;
    }
    if (state == State::Released || state == State::Failed) {
      return -ESHUTDOWN;
    }
    state = State::Sealing;
#ifdef WASMFS_OPFS_PROFILE_DRAIN_TEST
    wasmfs_opfs_profile_drain_test_note_sealing();
#endif
    operationCV.wait(lock, [&] { return activeOperations == 0; });
    assert(!drainState);
    drainState = this;
    return 0;
  }

  int finishDrain(bool success, Worker& proxy) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      assert(drainState == this);
      if (state != State::Sealing) {
        drainState = nullptr;
        return -ESHUTDOWN;
      }
      if (!success) {
        state = State::Failed;
        drainState = nullptr;
        return 0;
      }
    }

    int err = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_release_profile_lease(ctx.ctx, &err);
    });

    std::unique_lock<std::mutex> lock(mutex);
    assert(drainState == this);
    state = err == 0 ? State::Released : State::Failed;
    drainState = nullptr;
    return err;
  }
};

thread_local ProfileLeaseState* ProfileLeaseState::drainState = nullptr;
thread_local std::vector<ProfileLeaseState*> ProfileLeaseState::externalStates;

class OpenState {
public:
  enum Kind { None, Access, Blob, FailedAccessClose };

private:
  Kind kind = None;
  int id = -1;
  size_t openCount = 0;

public:
  Kind getKind() { return kind; }

  int open(Worker& proxy, int fileID, oflags_t flags) {
    if (kind == FailedAccessClose) {
      // A failed AccessHandle close has an ambiguous browser-side result. Do
      // not reuse it or attempt another close implicitly.
      return -EIO;
    }
    if (kind == None) {
      assert(openCount == 0);
      switch (flags) {
        case O_RDWR:
        case O_WRONLY:
          // If we need write access, try to open an AccessHandle.
          proxy(
            [&](auto ctx) { _wasmfs_opfs_open_access(ctx.ctx, fileID, &id); });
          // TODO: Fall back to open as a blob instead.
          if (id < 0) {
            return id;
          }
          kind = Access;
          break;
        case O_RDONLY:
          // We only need read access, so open as a Blob
          proxy(
            [&](auto ctx) { _wasmfs_opfs_open_blob(ctx.ctx, fileID, &id); });
          if (id < 0) {
            return id;
          }
          kind = Blob;
          break;
        default:
          WASMFS_UNREACHABLE("Unexpected open access mode");
      }
    } else if (kind == Blob && (flags == O_WRONLY || flags == O_RDWR)) {
      // Try to upgrade to an AccessHandle.
      int newID;
      proxy(
        [&](auto ctx) { _wasmfs_opfs_open_access(ctx.ctx, fileID, &newID); });
      if (newID < 0) {
        return newID;
      }
      // We have an AccessHandle, so close the blob.
      proxy([&]() { _wasmfs_opfs_close_blob(getBlobID()); });
      id = newID;
      kind = Access;
    }
    ++openCount;
    return 0;
  }

  int close(Worker& proxy) {
    // TODO: Downgrade to Blob access once the last writable file descriptor has
    // been closed.
    int err = 0;
    if (--openCount == 0) {
      switch (kind) {
        case Access:
          proxy(
            [&](auto ctx) { _wasmfs_opfs_close_access(ctx.ctx, id, &err); });
          break;
        case Blob:
          proxy([&]() { _wasmfs_opfs_close_blob(id); });
          break;
        case None:
          WASMFS_UNREACHABLE("Open file should have kind");
        case FailedAccessClose:
          WASMFS_UNREACHABLE("Failed close state should not be open");
      }
      if (kind == Access && err) {
        // JS deliberately retains this AccessHandle's slot after a rejected
        // close. Keep its ID here as a poison marker so this wrapper cannot
        // reopen or operate on a possibly live or already-closed handle.
        kind = FailedAccessClose;
      } else {
        kind = None;
        id = -1;
      }
    }
    return err;
  }

  int getAccessID() {
    assert(openCount > 0);
    assert(id >= 0);
    assert(kind == Access);
    return id;
  }

  int getBlobID() {
    assert(openCount > 0);
    assert(id >= 0);
    assert(kind == Blob);
    return id;
  }
};

class OPFSFile : public DataFile {
  // The JS FileSystemFileHandle is intentionally not retained while this file
  // is idle. Unlike the handle, this C++ object remains in the dcache so that
  // WasmFS file identity (including the pointer-derived inode number) stays
  // stable across a close and later reopen.
  Worker& proxy;
  int fileID = -1;
  int parentID;
  std::string name;
  OpenState state;
  std::shared_ptr<TerminalCloseState> terminalCloseState;
  std::shared_ptr<ProfileLeaseState> profileLeaseState;

  // The File mutex protects the open state, JS file-handle ID, and locator.
  // Keep the locator locally rather than deriving it from Directory::getName:
  // normal file operations already hold this mutex and must not acquire the
  // parent directory lock.
  int ensureFileID() {
    assert(fileID >= 0 || state.getKind() == OpenState::None);
    if (fileID >= 0) {
      return 0;
    }
    // removeChild clears the weak parent before dropping the dcache entry. Do
    // not reacquire a handle for an unlinked file whose directory ID may later
    // be reused in the JS allocator.
    auto currentParent = parent.lock();
    if (!currentParent) {
      return -ENOENT;
    }

    int newFileID = -1;
    proxy([&](auto ctx) {
      _wasmfs_opfs_acquire_file(
        ctx.ctx, parentID, name.c_str(), &newFileID);
    });
    if (newFileID < 0) {
      return newFileID;
    }
    fileID = newFileID;
    return 0;
  }

  void releaseFileIDIfIdle() {
    // FailedAccessClose is the intentional narrow exception: its ambiguous
    // SyncAccessHandle keeps the associated FileSystemFileHandle pinned until
    // wrapper teardown. Every healthy idle wrapper releases its JS reference.
    if (state.getKind() != OpenState::None || fileID < 0) {
      return;
    }
    proxy([&]() { _wasmfs_opfs_free_file(fileID); });
    fileID = -1;
  }

public:
  OPFSFile(mode_t mode,
           backend_t backend,
           int parentID,
           std::string name,
           Worker& proxy,
           std::shared_ptr<TerminalCloseState> terminalCloseState,
           std::shared_ptr<ProfileLeaseState> profileLeaseState)
    : DataFile(mode, backend), proxy(proxy), parentID(parentID),
      name(std::move(name)), terminalCloseState(std::move(terminalCloseState)),
      profileLeaseState(std::move(profileLeaseState)) {}

  ~OPFSFile() override {
    // A rejected AccessHandle close remains intentionally quarantined in the
    // ProxyWorker until its context is torn down. The file wrapper itself can
    // still be destroyed without pretending the close succeeded.
    assert(state.getKind() == OpenState::None ||
           state.getKind() == OpenState::FailedAccessClose);
    if (fileID >= 0) {
      proxy([&]() { _wasmfs_opfs_free_file(fileID); });
    }
  }

  int moveTo(int newParentID, const std::string& newName) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    if (state.getKind() == OpenState::FailedAccessClose) {
      return -EIO;
    }
    if (int err = ensureFileID()) {
      return err;
    }

    int err = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_move_file(
        ctx.ctx, fileID, newParentID, newName.c_str(), &err);
    });
    if (err == 0) {
      // Do not update this before the browser move succeeds: a later lazy
      // reacquisition must still name the old file after a failed move.
      parentID = newParentID;
      name = newName;
    }
    // A successful or failed move that began from an idle wrapper is a
    // one-shot file-handle operation. Keep a handle only when the file is
    // still open or deliberately poisoned by a failed access close.
    releaseFileIDIfIdle();
    return err;
  }

private:
  off_t getSize() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    off_t size;
    switch (state.getKind()) {
      case OpenState::None: {
        if (int err = ensureFileID()) {
          return err;
        }
        proxy([&](auto ctx) {
          _wasmfs_opfs_get_size_file(ctx.ctx, fileID, &size);
        });
        releaseFileIDIfIdle();
        break;
      }
      case OpenState::Access:
        proxy([&](auto ctx) {
          _wasmfs_opfs_get_size_access(ctx.ctx, state.getAccessID(), &size);
        });
        break;
      case OpenState::Blob:
        proxy([&]() { size = _wasmfs_opfs_get_size_blob(state.getBlobID()); });
        break;
      case OpenState::FailedAccessClose:
        return -EIO;
      default:
        WASMFS_UNREACHABLE("Unexpected open state");
    }
    return size;
  }

  int setSize(off_t size) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    int err = 0;
    switch (state.getKind()) {
      case OpenState::Access:
        proxy([&](auto ctx) {
          _wasmfs_opfs_set_size_access(
            ctx.ctx, state.getAccessID(), size, &err);
        });
        break;
      case OpenState::Blob:
        // We don't support `truncate` in blob mode because the blob would
        // become invalidated and refreshing it while ensuring other in-flight
        // operations on the same file do not observe the invalidated blob would
        // be extremely complicated.
        // TODO: Can we assume there are no other in-flight operations on this
        // file and do something better here?
        return -EIO;
      case OpenState::None: {
        if (int err = ensureFileID()) {
          return err;
        }
        proxy([&](auto ctx) {
          _wasmfs_opfs_set_size_file(ctx.ctx, fileID, size, &err);
        });
        releaseFileIDIfIdle();
        break;
      }
      case OpenState::FailedAccessClose:
        return -EIO;
      default:
        WASMFS_UNREACHABLE("Unexpected open state");
    }
    return err;
  }

  int open(oflags_t flags) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    if (state.getKind() == OpenState::FailedAccessClose) {
      return -EIO;
    }
    if (int err = ensureFileID()) {
      return err;
    }
    int err = state.open(proxy, fileID, flags);
    // Failed opens do not leave an open state to own an idle file handle.
    releaseFileIDIfIdle();
    return err;
  }

  int close() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    int err = state.close(proxy);
    if (err) {
      // __wasi_fd_close has already removed this descriptor's OpenFileState
      // before it calls us. Remember an ambiguous SyncAccessHandle close at
      // backend scope so terminalDrainFinished() retains a profile lease even
      // when the descriptor table is otherwise empty.
      terminalCloseState->recordFailedAccessClose(err);
    }
    // Keep a rejected close's FileSystemFileHandle only alongside its
    // quarantined SyncAccessHandle. A successful final close has no need for
    // an idle strong JS file-handle reference.
    releaseFileIDIfIdle();
    return err;
  }

  ssize_t read(uint8_t* buf, size_t len, off_t offset) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    // TODO: use an i64 here.
    int32_t nread;
    switch (state.getKind()) {
      case OpenState::Access:
        proxy([&]() {
          nread =
            _wasmfs_opfs_read_access(state.getAccessID(), buf, len, offset);
        });
        break;
      case OpenState::Blob:
        proxy([&](auto ctx) {
          _wasmfs_opfs_read_blob(
            ctx.ctx, state.getBlobID(), buf, len, offset, &nread);
        });
        break;
      case OpenState::FailedAccessClose:
        return -EIO;
      case OpenState::None:
      default:
        WASMFS_UNREACHABLE("Unexpected open state");
    }
    return nread;
  }

  ssize_t write(const uint8_t* buf, size_t len, off_t offset) override {
#ifdef WASMFS_OPFS_PROFILE_DRAIN_TEST
    wasmfs_opfs_profile_drain_test_maybe_block_file_operation();
#endif
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    if (state.getKind() == OpenState::FailedAccessClose) {
      return -EIO;
    }
    assert(state.getKind() == OpenState::Access);
    // TODO: use an i64 here.
    int32_t nwritten;
    proxy([&]() {
      nwritten =
        _wasmfs_opfs_write_access(state.getAccessID(), buf, len, offset);
    });
    return nwritten;
  }

  int flush() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    int err = 0;
    switch (state.getKind()) {
      case OpenState::Access:
        proxy([&](auto ctx) {
          _wasmfs_opfs_flush_access(ctx.ctx, state.getAccessID(), &err);
        });
        break;
      case OpenState::Blob:
      case OpenState::None:
      default:
        break;
      case OpenState::FailedAccessClose:
        return -EIO;
    }
    return err;
  }
};

class OPFSDirectory : public Directory {
public:
  Worker& proxy;

  // The ID of this directory in the JS library.
  int dirID = 0;
  std::shared_ptr<TerminalCloseState> terminalCloseState;
  std::shared_ptr<ProfileLeaseState> profileLeaseState;

  OPFSDirectory(mode_t mode,
                backend_t backend,
                int dirID,
                Worker& proxy,
                std::shared_ptr<TerminalCloseState> terminalCloseState,
                std::shared_ptr<ProfileLeaseState> profileLeaseState)
    : Directory(mode, backend), proxy(proxy), dirID(dirID),
      terminalCloseState(std::move(terminalCloseState)),
      profileLeaseState(std::move(profileLeaseState)) {}

  ~OPFSDirectory() override {
    // The root handle is shared by all mounts of this backend, so only child
    // directory handles are owned by their C++ wrappers. Slot 0 is reserved.
    if (dirID != 0 && dirID != kOPFSRootDirectoryID) {
      proxy([&]() { _wasmfs_opfs_free_directory(dirID); });
    }
  }

private:
  off_t getSize() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    return Directory::getSize();
  }

  std::shared_ptr<File> getChild(const std::string& name) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return nullptr;
    }
    int childType = 0, childID = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_get_child(
        ctx.ctx, dirID, name.c_str(), &childType, &childID);
    });
    if (childID < 0) {
      // TODO: More fine-grained error reporting.
      return NULL;
    }
    if (childType == 1) {
      return std::make_shared<OPFSFile>(
        0777,
        getBackend(),
        dirID,
        name,
        proxy,
        terminalCloseState,
        profileLeaseState);
    } else if (childType == 2) {
      return std::make_shared<OPFSDirectory>(
        0777,
        getBackend(),
        childID,
        proxy,
        terminalCloseState,
        profileLeaseState);
    } else {
      WASMFS_UNREACHABLE("Unexpected child type");
    }
  }

  std::shared_ptr<DataFile> insertDataFile(const std::string& name,
                                           mode_t mode) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return nullptr;
    }
    int childID = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_insert_file(ctx.ctx, dirID, name.c_str(), &childID);
    });
    if (childID < 0) {
      // TODO: Propagate specific errors.
      return nullptr;
    }
    return std::make_shared<OPFSFile>(
      mode,
      getBackend(),
      dirID,
      name,
      proxy,
      terminalCloseState,
      profileLeaseState);
  }

  std::shared_ptr<Directory> insertDirectory(const std::string& name,
                                             mode_t mode) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return nullptr;
    }
    int childID = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_insert_directory(ctx.ctx, dirID, name.c_str(), &childID);
    });
    if (childID < 0) {
      // TODO: Propagate specific errors.
      return nullptr;
    }
    return std::make_shared<OPFSDirectory>(
      mode,
      getBackend(),
      childID,
      proxy,
      terminalCloseState,
      profileLeaseState);
  }

  std::shared_ptr<Symlink> insertSymlink(const std::string& name,
                                         const std::string& target) override {
    // Symlinks not supported.
    // TODO: Propagate EPERM specifically.
    return nullptr;
  }

  int insertMove(const std::string& name, std::shared_ptr<File> file) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    if (file->is<DataFile>()) {
      auto opfsFile = std::static_pointer_cast<OPFSFile>(file);
      return opfsFile->moveTo(dirID, name);
    } else {
      // TODO: Support moving directories once OPFS supports that.
      // EBUSY can be returned when the directory is "in use by the system,"
      // which can mean whatever we want.
      return -EBUSY;
    }
  }

  int removeChild(const std::string& name) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    int err = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_remove_child(ctx.ctx, dirID, name.c_str(), &err);
    });
    return err;
  }

  ssize_t getNumEntries() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    auto entries = getEntries();
    if (int err = entries.getError()) {
      return err;
    }
    return entries->size();
  }

  Directory::MaybeEntries getEntries() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return {operation.getError()};
    }
    std::vector<Directory::Entry> entries;
    int err = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_get_entries(ctx.ctx, dirID, &entries, &err);
    });
    if (err) {
      assert(err < 0);
      return {err};
    }
    return {entries};
  }
};

class OPFSBackend : public Backend {
public:
  Worker proxy;

  bool supportsExplicitMetadataMutation() const override { return false; }

  bool supportsRecordLocks() const override {
    // A default OPFS backend has no cross-instance lock domain. A successful
    // record lock is safe only after this backend has acquired the cooperative
    // storage-bucket profile lease.
    return profileLeaseState->supportsRecordLocks();
  }

  int acquireProfileOperation() override {
    return profileLeaseState->acquireExternalOperation();
  }

  void releaseProfileOperation() override {
    profileLeaseState->releaseExternalOperation();
  }

  bool isLeasedOPFSProfileBackend() const override {
    return profileLeaseState->isLeasedProfileBackend();
  }

  int beginOPFSProfileDrain() override {
    return profileLeaseState->beginDrain();
  }

  int finishOPFSProfileDrain(bool success) override {
    return profileLeaseState->finishDrain(success, proxy);
  }

  int getOPFSProfilePriorCloseError() const override {
    return terminalCloseState->getFailedAccessCloseError();
  }

  int terminalDrainFinished(bool success) override {
    // The generic drain has already recorded a descriptor flush/close error.
    // Retain the lease without reporting the same failed close a second time
    // through the backend-finalizer counter. A close that failed before drain
    // begins is different: it is absent from FileTable and must be surfaced by
    // the backend latch below while `success` is still true.
    if (!profileLeaseState->hasLiveLease()) {
      return 0;
    }

    int beginError = profileLeaseState->beginDrain();
    // A scoped profile drain already sealed this backend. It either released
    // the lease or deliberately retained it in Failed state; global terminal
    // teardown must never retry that ambiguous release. Do still surface the
    // failed scoped handoff: returning success here would hide a live profile
    // lease behind an apparently successful terminal result.
    if (beginError == -ESHUTDOWN) {
      return profileLeaseState->hasFailedDrain() ? -ESHUTDOWN : 0;
    }
    if (beginError) {
      return beginError;
    }

    if (!success) {
      return profileLeaseState->finishDrain(false, proxy);
    }

    if (int error = terminalCloseState->getFailedAccessCloseError()) {
      // An ordinary close can fail before terminalDrain begins, after which
      // its descriptor has already left FileTable. Surface that terminal
      // resource failure and retain any cooperative lease rather than treating
      // an empty table as a safe browser-side handoff.
      (void)profileLeaseState->finishDrain(false, proxy);
      return error;
    }

    return profileLeaseState->finishDrain(true, proxy);
  }

  bool releasesTerminalLease() const override {
    return profileLeaseState->hasLiveLease();
  }

  ~OPFSBackend() override {
    // Only an explicit, successful terminal or scoped profile drain may
    // acknowledge release of a cooperative profile lease. In particular, a
    // backend destructor does not know whether its file states and libc
    // streams were durably drained, so it must not turn context teardown into
    // a false success. Browser Web Locks may still release when the worker
    // context itself exits; that is not a durability acknowledgement.
  }

  int acquireProfileLease(const std::string& profileName) {
    int err = 0;
    proxy([&](auto ctx) {
      _wasmfs_opfs_acquire_profile_lease(ctx.ctx, profileName.c_str(), &err);
    });
    if (err == 0) {
      profileLeaseState->acquiredLease();
    }
    return err;
  }

  std::shared_ptr<DataFile> createFile(mode_t mode) override {
    // No way to support a raw file without a parent directory.
    // TODO: update the core system to document this as a possible result of
    // `createFile` and to handle it gracefully.
    return nullptr;
  }

  std::shared_ptr<Directory> createDirectory(mode_t mode) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return nullptr;
    }
    proxy([](auto ctx) { _wasmfs_opfs_init_root_directory(ctx.ctx); });
    return std::make_shared<OPFSDirectory>(
      mode,
      this,
      kOPFSRootDirectoryID,
      proxy,
      terminalCloseState,
      profileLeaseState);
  }

  std::shared_ptr<Symlink> createSymlink(std::string target) override {
    // Symlinks not supported.
    return nullptr;
  }

private:
  std::shared_ptr<TerminalCloseState> terminalCloseState =
    std::make_shared<TerminalCloseState>();
  std::shared_ptr<ProfileLeaseState> profileLeaseState =
    std::make_shared<ProfileLeaseState>();
};

} // anonymous namespace

extern "C" {

backend_t wasmfs_create_opfs_backend() {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    errno = operation.getError();
    return NullBackend;
  }
  // ProxyWorker cannot safely be synchronously spawned from the main browser
  // thread. See comment in thread_utils.h for more details.
  assert(
    !emscripten_is_main_browser_thread() ||
    emscripten_has_asyncify() &&
      "Cannot safely create OPFS backend on main browser thread without Asyncify or JSPI");

  return wasmFS.addBackend(std::make_unique<OPFSBackend>());
}

backend_t wasmfs_create_opfs_backend_with_profile_lease(
  const char* profile_name) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    errno = operation.getError();
    return NullBackend;
  }
  if (!IsValidProfileLeaseName(profile_name)) {
    errno = EINVAL;
    return nullptr;
  }

#ifndef __EMSCRIPTEN_PTHREADS__
  // A lease must live in the dedicated OPFS worker that owns the backend.
  // JSPI and Asyncify use the caller's JS realm instead, so they cannot safely
  // provide independent per-backend lease ownership.
  errno = ENOTSUP;
  return nullptr;
#else
  // Reserve before constructing ProxyWorker or asking Web Locks. One WasmFS
  // instance has one terminal-drain lease handoff, so a second leased backend
  // must fail locally rather than acquiring an unrelated browser lock that
  // terminalDrain cannot coordinate transactionally.
  if (!wasmFS.reserveTerminalLeaseOwner()) {
    errno = EBUSY;
    return NullBackend;
  }

  // The lease is obtained by the dedicated worker before this backend has
  // initialized an OPFS directory or file handle.
  assert(
    !emscripten_is_main_browser_thread() &&
    "Cannot safely create leased OPFS backend on main browser thread");

  auto backend = std::make_unique<OPFSBackend>();
  int err = backend->acquireProfileLease(profile_name);
  if (err != 0) {
    assert(err < 0);
    wasmFS.cancelTerminalLeaseOwnerReservation();
    errno = -err;
    return nullptr;
  }
  return wasmFS.addBackend(std::move(backend));
#endif
}

void EMSCRIPTEN_KEEPALIVE _wasmfs_opfs_record_entry(
  std::vector<Directory::Entry>* entries, const char* name, int type) {
  entries->push_back({name, File::FileKind(type), 0});
}

} // extern "C"
