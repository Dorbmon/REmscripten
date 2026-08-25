// Copyright 2022 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <emscripten/threading.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <errno.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdlib.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

#ifdef WASMFS_OPFS_TEST_GET_CHILD_PROXY_FAILURE
constexpr char kOPFSGetChildProxyFailureTestName[] =
  "__wasmfs_opfs_test_get_child_proxy_failure__";
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE
#define WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE 0
#endif

#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE < 0 || \
  WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE > 4
#error "invalid profile namespace initialisation failure test selector"
#endif

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

  template<typename T> bool operator()(T func) { return proxy(func); }

  em_proxying_queue* getQueue() const { return proxy.getQueue(); }
  int fenceForRetirement() { return proxy.fenceForRetirement(); }
  void markHeartbeatStopped() { proxy.markHeartbeatStopped(); }
  int retire() { return proxy.retire(); }
  void abandonScopedProfileWorker() { proxy.abandonScopedProfileWorker(); }
#else
  // When used with JSPI on the main thread the various wasmfs_opfs_* functions
  // can be directly executed since they are all async.
  template<typename T> bool operator()(T func) {
    if constexpr (std::is_invocable_v<T&, ProxyingQueue::ProxyingCtx>) {
      // TODO: Find a way to remove this, since it's unused.
      ProxyingQueue::ProxyingCtx p;
      func(p);
    } else {
      func();
    }
    return true;
  }

  em_proxying_queue* getQueue() const { return nullptr; }
  int fenceForRetirement() { return -ENOTSUP; }
  void markHeartbeatStopped() {}
  int retire() { return -ENOTSUP; }
  void abandonScopedProfileWorker() {}
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
  enum class State {
    Unleased,
    Active,
    Sealing,
    Released,
    Retiring,
    Retired,
    Failed,
  };

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

  // Cached OPFS File/Directory wrappers can be dropped independently of an
  // outer WasmFS syscall. Their destructors normally proxy a handle free to
  // the dedicated Worker, so give those proxies a separate admission guard.
  // Scoped retirement closes this gate while Sealing and waits for every
  // already-admitted destructor proxy before it resets the worker-local JS
  // allocators. Later destructors then become intentional no-ops over the
  // sealed backend tombstone.
  class DestructorProxyOperation {
    ProfileLeaseState& state;
    bool admitted = false;

  public:
    explicit DestructorProxyOperation(ProfileLeaseState& state) : state(state) {
      std::unique_lock<std::mutex> lock(state.mutex);
      if (state.destructorProxyingClosed) {
        return;
      }
      ++state.destructorProxyOperations;
      admitted = true;
    }

    DestructorProxyOperation(const DestructorProxyOperation&) = delete;
    DestructorProxyOperation& operator=(const DestructorProxyOperation&) =
      delete;

    ~DestructorProxyOperation() {
      if (!admitted) {
        return;
      }
      std::unique_lock<std::mutex> lock(state.mutex);
      assert(state.destructorProxyOperations > 0);
      --state.destructorProxyOperations;
      if (state.destructorProxyOperations == 0) {
        state.destructorProxyCV.notify_all();
      }
    }

    explicit operator bool() const { return admitted; }
  };

  struct ScopedFinishResult {
    int error = 0;
    bool leaseReleased = false;
  };

private:
  mutable std::mutex mutex;
  std::condition_variable operationCV;
  std::condition_variable destructorProxyCV;
  State state = State::Unleased;
  size_t activeOperations = 0;
  size_t destructorProxyOperations = 0;
  bool destructorProxyingClosed = false;

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

  // A failed scoped transaction either still owns the lease (Failed) or has
  // released it but did not complete worker retirement (Retiring). Neither
  // state is a successful terminal handoff, and a later terminal drain must
  // report it rather than silently retrying or returning success.
  bool hasIncompleteScopedDrain() const {
    std::unique_lock<std::mutex> lock(mutex);
    return state == State::Failed || state == State::Retiring;
  }

  // Retiring has no live Web Lock, but it still needs the lease-owner finalizer
  // slot so terminal drain can surface its prior incomplete handoff.
  bool needsTerminalLeaseFinalizer() const {
    std::unique_lock<std::mutex> lock(mutex);
    return state == State::Active || state == State::Sealing ||
           state == State::Failed || state == State::Retiring;
  }

  void closeDestructorProxying() {
    std::unique_lock<std::mutex> lock(mutex);
    destructorProxyingClosed = true;
    destructorProxyCV.wait(
      lock, [&] { return destructorProxyOperations == 0; });
  }

  bool isDestructorProxyingClosed() {
    std::unique_lock<std::mutex> lock(mutex);
    return destructorProxyingClosed;
  }

  int beginDrain() {
    std::unique_lock<std::mutex> lock(mutex);
    if (state == State::Unleased) {
      return -ENOTSUP;
    }
    if (state == State::Sealing) {
      return -EBUSY;
    }
    if (state == State::Released || state == State::Retiring ||
        state == State::Retired || state == State::Failed) {
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

  ScopedFinishResult finishScopedDrain(Worker& proxy) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      assert(drainState == this);
      if (state != State::Sealing) {
        drainState = nullptr;
        return {-ESHUTDOWN, false};
      }
    }

    int error = 0;
    int released = 0;
    bool proxied = proxy([&](auto ctx) {
      _wasmfs_opfs_release_profile_lease_and_retire_context(
        ctx.ctx, proxy.getQueue(), &released, &error);
    });
    if (!proxied) {
      error = -EIO;
      // A JS callback can write the release witness and then be interrupted
      // before it resets allocators/stops the heartbeat/finishes its proxying
      // context. Only proxy completion acknowledges the whole transaction.
      // Do not free the queue or claim a release in that ambiguous state.
      released = 0;
    }

    std::unique_lock<std::mutex> lock(mutex);
    assert(drainState == this);
    // The JS transaction reports release separately because it can encounter
    // a cleanup error only after Web Locks has acknowledged handoff.
    state = released ? State::Released : State::Failed;
    drainState = nullptr;
    return {error, released != 0};
  }

  int beginRetirement() {
    std::unique_lock<std::mutex> lock(mutex);
    if (state != State::Released) {
      return -ESHUTDOWN;
    }
    assert(destructorProxyingClosed);
    assert(destructorProxyOperations == 0);
    state = State::Retiring;
    return 0;
  }

  void finishRetirement(bool success) {
    std::unique_lock<std::mutex> lock(mutex);
    assert(state == State::Retiring);
    if (success) {
      state = State::Retired;
    }
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
  Kind getKind() const { return kind; }

  int open(Worker& proxy, int fileID, oflags_t flags) {
    if (kind == FailedAccessClose) {
      // A failed browser-handle transition has an ambiguous browser-side
      // result. Do not reuse it or attempt another close implicitly.
      return -EIO;
    }
    if (kind == None) {
      assert(openCount == 0);
      switch (flags) {
        case O_RDWR:
        case O_WRONLY:
          // If we need write access, try to open an AccessHandle.
          {
            int newID = -EIO;
            if (!proxy([&](auto ctx) {
                  _wasmfs_opfs_open_access(ctx.ctx, fileID, &newID);
                })) {
              kind = FailedAccessClose;
              return -EIO;
            }
            // TODO: Fall back to open as a blob instead.
            if (newID < 0) {
              return newID;
            }
            id = newID;
          }
          // TODO: Fall back to open as a blob instead.
          kind = Access;
          break;
        case O_RDONLY:
          // We only need read access, so open as a Blob
          {
            int newID = -EIO;
            if (!proxy([&](auto ctx) {
                  _wasmfs_opfs_open_blob(ctx.ctx, fileID, &newID);
                })) {
              kind = FailedAccessClose;
              return -EIO;
            }
            if (newID < 0) {
              return newID;
            }
            id = newID;
          }
          kind = Blob;
          break;
        default:
          WASMFS_UNREACHABLE("Unexpected open access mode");
      }
    } else if (kind == Blob && (flags == O_WRONLY || flags == O_RDWR)) {
      // Try to upgrade to an AccessHandle.
      int newID = -EIO;
      if (!proxy([&](auto ctx) {
            _wasmfs_opfs_open_access(ctx.ctx, fileID, &newID);
          })) {
        kind = FailedAccessClose;
        return -EIO;
      }
      if (newID < 0) {
        return newID;
      }
      // We have an AccessHandle, so close the blob.
      if (!proxy([&]() { _wasmfs_opfs_close_blob(getBlobID()); })) {
        // The browser may have opened the access handle and may or may not
        // have closed the blob. Quarantine this wrapper until terminal
        // retirement verifies that no browser-owned handle remains.
        id = newID;
        kind = FailedAccessClose;
        return -EIO;
      }
      id = newID;
      kind = Access;
    }
    ++openCount;
    return 0;
  }

  int close(Worker& proxy) {
    // TODO: Downgrade to Blob access once the last writable file descriptor has
    // been closed.
    assert(openCount > 0);
    int err = 0;
    if (--openCount == 0) {
      switch (kind) {
        case Access:
          if (!proxy([&](auto ctx) {
                _wasmfs_opfs_close_access(ctx.ctx, id, &err);
              })) {
            err = -EIO;
          }
          break;
        case Blob:
          if (!proxy([&]() { _wasmfs_opfs_close_blob(id); })) {
            err = -EIO;
          }
          break;
        case None:
          WASMFS_UNREACHABLE("Open file should have kind");
        case FailedAccessClose:
          // An earlier failed upgrade can leave an already-open descriptor
          // above this one. Its final close must be observable, but it must
          // never perform another ambiguous browser-handle operation.
          return -EIO;
      }
      if (err) {
        // Keep the handle ID as a poison marker so this wrapper cannot reopen
        // or operate on a possibly live or already-closed browser resource.
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

  // A cancelled proxy operation does not tell us whether the browser-side
  // operation ran.  Do not let a later operation (especially a close) make a
  // second request against that ambiguous handle state.
  void poison() { kind = FailedAccessClose; }
};

class OPFSFile : public DataFile {
  // The JS FileSystemFileHandle is intentionally not retained while this file
  // is idle. Unlike the handle, this C++ object remains in the dcache so that
  // WasmFS file identity (including the pointer-derived inode number) stays
  // stable across a close and later reopen.
  Worker& proxy;
  int fileID = -1;
  // A cancelled file-handle release is just as ambiguous as a cancelled
  // SyncAccessHandle close. Preserve the ID for terminal preflight and reject
  // future operations instead of inventing an idle, reusable handle.
  bool failedFileHandleRelease = false;
  int parentID;
  std::string name;
  OpenState state;
  std::shared_ptr<TerminalCloseState> terminalCloseState;
  std::shared_ptr<ProfileLeaseState> profileLeaseState;

  int recordProxyFailure() {
    // This covers cancelled access/blob operations as well as a cancelled
    // idle FileSystemFileHandle operation.  A later close cannot safely
    // distinguish an operation that never reached the worker from one whose
    // completion acknowledgement was lost.
    state.poison();
    terminalCloseState->recordFailedAccessClose(-EIO);
    return -EIO;
  }

  bool hasUnacknowledgedBrowserOperation() const {
    return state.getKind() == OpenState::FailedAccessClose ||
           failedFileHandleRelease;
  }

  // The File mutex protects the open state, JS file-handle ID, and locator.
  // Keep the locator locally rather than deriving it from Directory::getName:
  // normal file operations already hold this mutex and must not acquire the
  // parent directory lock.
  int ensureFileID() {
    assert(fileID >= 0 || state.getKind() == OpenState::None);
    if (failedFileHandleRelease) {
      return -EIO;
    }
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

    int newFileID = -EIO;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_acquire_file(
            ctx.ctx, parentID, name.c_str(), &newFileID);
        })) {
      return recordProxyFailure();
    }
    if (newFileID < 0) {
      return newFileID;
    }
    fileID = newFileID;
    return 0;
  }

  int releaseFileIDIfIdle() {
    // FailedAccessClose is the intentional narrow exception: its ambiguous
    // SyncAccessHandle keeps the associated FileSystemFileHandle pinned until
    // wrapper teardown. Every healthy idle wrapper releases its JS reference.
    if (state.getKind() != OpenState::None || fileID < 0) {
      return 0;
    }
    if (!proxy([&]() { _wasmfs_opfs_free_file(fileID); })) {
      failedFileHandleRelease = true;
      return recordProxyFailure();
    }
    fileID = -1;
    return 0;
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
    ProfileLeaseState::DestructorProxyOperation destructorOperation(
      *profileLeaseState);
    // Once a proxy completion is lost, even a best-effort free would be a
    // second ambiguous operation.  The scoped worker tombstone owns that
    // browser resource until document teardown instead.
    if (destructorOperation && fileID >= 0 &&
        state.getKind() == OpenState::None && !failedFileHandleRelease) {
      if (!proxy([&]() { _wasmfs_opfs_free_file(fileID); })) {
        terminalCloseState->recordFailedAccessClose(-EIO);
      }
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
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_move_file(
            ctx.ctx, fileID, newParentID, newName.c_str(), &err);
        })) {
      err = recordProxyFailure();
    }
    if (err == 0) {
      // Do not update this before the browser move succeeds: a later lazy
      // reacquisition must still name the old file after a failed move.
      parentID = newParentID;
      name = newName;
    }
    // A successful or failed move that began from an idle wrapper is a
    // one-shot file-handle operation. Keep a handle only when the file is
    // still open or deliberately poisoned by a failed access close.
    int releaseError = releaseFileIDIfIdle();
    return err ? err : releaseError;
  }

private:
  off_t getSize() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    off_t size = -EIO;
    switch (state.getKind()) {
      case OpenState::None: {
        if (int err = ensureFileID()) {
          return err;
        }
        if (!proxy([&](auto ctx) {
              _wasmfs_opfs_get_size_file(ctx.ctx, fileID, &size);
            })) {
          return recordProxyFailure();
        }
        if (int err = releaseFileIDIfIdle()) {
          return err;
        }
        break;
      }
      case OpenState::Access:
        if (!proxy([&](auto ctx) {
              _wasmfs_opfs_get_size_access(
                ctx.ctx, state.getAccessID(), &size);
            })) {
          return recordProxyFailure();
        }
        break;
      case OpenState::Blob:
        if (!proxy([&]() {
              size = _wasmfs_opfs_get_size_blob(state.getBlobID());
            })) {
          return recordProxyFailure();
        }
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
        if (!proxy([&](auto ctx) {
              _wasmfs_opfs_set_size_access(
                ctx.ctx, state.getAccessID(), size, &err);
            })) {
          return recordProxyFailure();
        }
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
        if (!proxy([&](auto ctx) {
              _wasmfs_opfs_set_size_file(ctx.ctx, fileID, size, &err);
            })) {
          return recordProxyFailure();
        }
        if (int releaseError = releaseFileIDIfIdle()) {
          return releaseError;
        }
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
    int releaseError = releaseFileIDIfIdle();
    if (err && hasUnacknowledgedBrowserOperation()) {
      // A completed browser-side open rejection has no live handle to carry
      // into retirement.  Only a lost proxy acknowledgement (or a failed
      // follow-up file-handle release) needs to poison the backend-wide
      // handoff state.
      terminalCloseState->recordFailedAccessClose(err);
    }
    return err ? err : releaseError;
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
    if (int releaseError = releaseFileIDIfIdle()) {
      if (!err) {
        err = releaseError;
        terminalCloseState->recordFailedAccessClose(err);
      }
    }
    return err;
  }

  ssize_t read(uint8_t* buf, size_t len, off_t offset) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    // TODO: use an i64 here.
    int32_t nread = -EIO;
    switch (state.getKind()) {
      case OpenState::Access:
        if (!proxy([&]() {
              nread = _wasmfs_opfs_read_access(
                state.getAccessID(), buf, len, offset);
            })) {
          return recordProxyFailure();
        }
        break;
      case OpenState::Blob:
        if (!proxy([&](auto ctx) {
              _wasmfs_opfs_read_blob(
                ctx.ctx, state.getBlobID(), buf, len, offset, &nread);
            })) {
          return recordProxyFailure();
        }
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
    int32_t nwritten = -EIO;
    if (!proxy([&]() {
          nwritten = _wasmfs_opfs_write_access(
            state.getAccessID(), buf, len, offset);
        })) {
      return recordProxyFailure();
    }
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
        if (!proxy([&](auto ctx) {
              _wasmfs_opfs_flush_access(
                ctx.ctx, state.getAccessID(), &err);
            })) {
          return recordProxyFailure();
        }
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
    ProfileLeaseState::DestructorProxyOperation destructorOperation(
      *profileLeaseState);
    if (destructorOperation && dirID != 0 && dirID != kOPFSRootDirectoryID) {
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
    auto child = getChildWithError(name);
    if (child.getError()) {
      return nullptr;
    }
    return child.getFile();
  }

  Directory::MaybeFile getChildWithError(const std::string& name) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    // A cancelled proxy must not leave the default output values looking like
    // a successful lookup. The JS side uses a negative child ID for every
    // completed lookup failure; reserve the same fail-closed sentinel here
    // until the proxy confirms that it ran.
    int childType = 0;
    int childID = -EIO;
    bool proxyCompleted = false;
#ifdef WASMFS_OPFS_TEST_GET_CHILD_PROXY_FAILURE
    if (name != kOPFSGetChildProxyFailureTestName)
#endif
    {
      proxyCompleted = proxy([&](auto ctx) {
        _wasmfs_opfs_get_child(
          ctx.ctx, dirID, name.c_str(), &childType, &childID);
      });
    }
    if (!proxyCompleted) {
      return -EIO;
    }
    if (childID < 0) {
      if (childID == -ENOENT) {
        return std::shared_ptr<File>();
      }
      return childID;
    }
    if (childType == 1 && childID == 0) {
      return std::make_shared<OPFSFile>(
        0777,
        getBackend(),
        dirID,
        name,
        proxy,
        terminalCloseState,
        profileLeaseState);
    }
    if (childType == 2 && childID > kOPFSRootDirectoryID) {
      return std::make_shared<OPFSDirectory>(
        0777,
        getBackend(),
        childID,
        proxy,
        terminalCloseState,
        profileLeaseState);
    }
    // Neither a malformed JS result nor a cancelled/failed proxy is a missing
    // child. Propagate EIO to the syscall layer instead of trapping or
    // fabricating ENOENT.
    return -EIO;
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

  int prepareOPFSProfileRetirement(bool checkResources) override {
    // This runs only after WasmFS has recorded the one-way sealed result bit.
    // Close/wait the destructor proxy gate before the worker fence.  A fence
    // failure leaves this backend sealed, so cached File/Directory destructors
    // must already be unable to enqueue into a Worker that can later be
    // abandoned rather than joined.
    profileLeaseState->closeDestructorProxying();

    // Fence before any later scoped cleanup can fail.  The gate above remains
    // closed on every failure path; JS allocators are not reset until the
    // release-and-retire transaction below acknowledges success.
    if (int error = proxy.fenceForRetirement()) {
      return error;
    }
    if (!checkResources) {
      return 0;
    }
    int error = 0;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_prepare_profile_retirement(ctx.ctx, &error);
        })) {
      return -EIO;
    }
    return error;
  }

  int finishOPFSProfileDrain(bool success, bool* leaseReleased) override {
    assert(leaseReleased);
    *leaseReleased = false;
    if (!success) {
      int error = profileLeaseState->finishDrain(false, proxy);
      // prepareOPFSProfileRetirement() closes this gate before attempting the
      // browser-main fence. The failure fallback below retains a live queue,
      // but no cached OPFS File/Directory destructor may enqueue into it.
      assert(profileLeaseState->isDestructorProxyingClosed());
      proxy.abandonScopedProfileWorker();
      return error;
    }

    auto result = profileLeaseState->finishScopedDrain(proxy);
    *leaseReleased = result.leaseReleased;
    if (!result.leaseReleased) {
      // The transaction did not acknowledge a safe Web Locks release. Keep
      // its worker/heartbeat as a fail-closed tombstone (an interrupted
      // callback can leave physical release indeterminate). This detaches on
      // the application pthread and deliberately retains queue storage.
      assert(profileLeaseState->isDestructorProxyingClosed());
      proxy.abandonScopedProfileWorker();
    } else {
      // The transaction only reports release after it also reset OPFS state
      // and cleared its worker-local heartbeat.
      proxy.markHeartbeatStopped();
    }
    return result.error;
  }

  int retireOPFSProfileBackend(bool transactionSucceeded) override {
    if (int error = profileLeaseState->beginRetirement()) {
      return error;
    }
    int error = proxy.retire();
    profileLeaseState->finishRetirement(error == 0 && transactionSucceeded);
    return error;
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
    if (profileLeaseState->hasIncompleteScopedDrain()) {
      // Do not retry either an ambiguous retained lease or an already
      // released-but-incompletely-retired worker. Both are terminal failures
      // of the earlier scoped handoff and must remain visible to the caller.
      return -ESHUTDOWN;
    }
    if (!profileLeaseState->hasLiveLease()) {
      return 0;
    }

    int beginError = profileLeaseState->beginDrain();
    // A scoped profile drain already sealed this backend. Its Failed state has
    // no acknowledged safe release (and an interrupted callback can be
    // physically indeterminate); Retiring has an acknowledged release but an
    // incomplete worker handoff. Global terminal teardown must never retry
    // either state or report it as a success.
    if (beginError == -ESHUTDOWN) {
      return profileLeaseState->hasIncompleteScopedDrain() ? -ESHUTDOWN : 0;
    }
    if (beginError) {
      return beginError;
    }

    int firstError = 0;
    bool cleanupSucceeded = success;
    if (cleanupSucceeded &&
        (firstError = terminalCloseState->getFailedAccessCloseError())) {
      // An ordinary close can fail before terminalDrain begins, after which
      // its descriptor has already left FileTable. Surface that terminal
      // resource failure and retain any cooperative lease rather than treating
      // an empty table as a safe browser-side handoff.
      cleanupSucceeded = false;
    }

    // Terminal profile release has the same Worker lifetime requirement as a
    // scoped release: global WasmFS destruction can run on browser main after
    // EXIT_RUNTIME, so it must never be left to join this Worker. Use the
    // exact preflight, one-worker release/reset/heartbeat-stop transaction,
    // and application-pthread retirement sequence used by the scoped API.
    if (int error = prepareOPFSProfileRetirement(cleanupSucceeded)) {
      firstError = firstError ? firstError : error;
      cleanupSucceeded = false;
    }

    bool leaseReleased = false;
    if (int error = finishOPFSProfileDrain(cleanupSucceeded, &leaseReleased)) {
      firstError = firstError ? firstError : error;
    }
    if (!leaseReleased) {
      // Do not let an implementation omission turn a completed terminal
      // cleanup into a false success without an acknowledged lease handoff.
      // If another terminal finalizer already failed, this backend has not
      // acknowledged a new release; do not manufacture a second error.
      return firstError ? firstError : cleanupSucceeded ? -EIO : 0;
    }

    // A cleanup error after release still needs native Worker retirement to
    // make the eventual global destructor browser-main-safe. Passing false
    // preserves Retiring and a non-success terminal result.
    if (int error = retireOPFSProfileBackend(firstError == 0)) {
      firstError = firstError ? firstError : error;
    }
    return firstError;
  }

  bool releasesTerminalLease() const override {
    return profileLeaseState->needsTerminalLeaseFinalizer();
  }

  ~OPFSBackend() override {
    // Only an explicit, successful terminal or scoped profile drain may
    // acknowledge release of a cooperative profile lease. In particular, a
    // backend destructor does not know whether its file states and libc
    // streams were durably drained, so it must not turn context teardown into
    // a false success. Browser Web Locks may still release when the worker
    // context itself exits; that is not a durability acknowledgement.
  }

  int acquireProfileLease(const std::string& profileName,
                          bool* proxyCompleted = nullptr) {
    if (proxyCompleted) {
      *proxyCompleted = false;
    }
    int err = 0;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_acquire_profile_lease(
            ctx.ctx, profileName.c_str(), &err);
        })) {
      return -EIO;
    }
    if (proxyCompleted) {
      *proxyCompleted = true;
    }
    if (err == 0) {
      profileLeaseState->acquiredLease();
    }
    return err;
  }

  // A cancelled acquire callback can have reached the browser after native
  // proxying lost its completion acknowledgement. There is no safe explicit
  // release in that state because ProfileLeaseState never observed a lease.
  // Retain this dedicated worker until document teardown and keep WasmFS's
  // terminal-owner reservation so the same instance cannot retry blindly.
  void abandonUnacknowledgedProfileLeaseAcquire() {
    proxy.abandonScopedProfileWorker();
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

protected:
  std::shared_ptr<TerminalCloseState> terminalCloseState =
    std::make_shared<TerminalCloseState>();
  std::shared_ptr<ProfileLeaseState> profileLeaseState =
    std::make_shared<ProfileLeaseState>();
};

// A direct OPFS directory cannot give a caller a durable directory rename or
// directory fsync: both are explicitly unsupported by the browser-backed
// implementation above.  The profile namespace backend therefore stores one
// logical tree in an opaque regular OPFS file.  Every successful mutation
// appends a complete tree image, flushes it, then publishes an alternate
// checksummed selector and flushes that selector.  Recovery chooses the
// highest complete selector.  This deliberately favors a small, sharp
// correctness boundary over profile-scale performance.
constexpr size_t kProfileNamespaceSuperblockSize = 4096;
constexpr uint64_t kProfileNamespacePayloadOffset =
  2 * kProfileNamespaceSuperblockSize;
constexpr uint32_t kProfileNamespaceFormatVersion = 1;
// The permanent bootstrap journal has two alternating fixed-size records.
// It is intentionally separate from the logical namespace container: a
// selector in the container cannot tell a fresh factory whether a missing or
// malformed physical container was never published or was a corrupt profile.
constexpr size_t kProfileNamespaceJournalRecordSize = 64;
constexpr size_t kProfileNamespaceJournalSize =
  2 * kProfileNamespaceJournalRecordSize;
constexpr uint32_t kProfileNamespaceJournalFormatVersion = 1;
constexpr size_t kMaxProfileNamespacePayloadBytes = 16 * 1024 * 1024;
constexpr size_t kMaxProfileNamespaceContainerBytes = 64 * 1024 * 1024;
constexpr size_t kMaxProfileNamespaceNodes = 65536;
// The initial recovery format intentionally bounds tree depth as well as
// total nodes.  Both serialization and parsing recurse once per directory;
// accepting an attacker/corruption-controlled 65K-deep tree would turn a
// malformed persisted profile into a native stack overflow rather than EIO.
constexpr size_t kMaxProfileNamespaceDepth = 256;
constexpr size_t kProfileNamespaceNameMax = 255;
constexpr std::array<uint8_t, 8> kProfileNamespaceSuperblockMagic = {
  'W', 'F', 'S', 'N', 'S', 'P', '0', '1'};
constexpr std::array<uint8_t, 8> kProfileNamespacePayloadMagic = {
  'W', 'F', 'S', 'N', 'P', 'T', '0', '1'};
constexpr std::array<uint8_t, 8> kProfileNamespaceJournalMagic = {
  'W', 'F', 'S', 'N', 'J', 'R', '0', '1'};

#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
// Test-only controlled-interruption hook. Phases 1 and 2 surround an
// alternate container-selector publication; phase 3 runs after PREPARED;
// phases 4 and 5 run after the first PUBLISHED copy and its quorum mirror.
// Production builds do not contain this call site.
extern "C" void wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(
  int phase);
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION
#define WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION 0
#endif

#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION > 2
#error "WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION must be 0, 1, or 2"
#endif

enum class ProfileNamespaceNodeKind : uint8_t {
  File = 1,
  Directory = 2,
};

enum class ProfileNamespaceBootstrapState : uint32_t {
  Prepared = 1,
  Published = 2,
};

struct ProfileNamespaceNode {
  ProfileNamespaceNodeKind kind;
  uint64_t id;
  mode_t mode;
  std::vector<uint8_t> data;
  std::map<std::string, std::shared_ptr<ProfileNamespaceNode>> children;
  std::weak_ptr<ProfileNamespaceNode> parent;

  ProfileNamespaceNode(ProfileNamespaceNodeKind kind, uint64_t id, mode_t mode)
    : kind(kind), id(id), mode(mode) {}
};

class ProfileNamespaceDataFile;
class ProfileNamespaceDirectory;

class ProfileNamespaceBackend : public OPFSBackend {
  struct Superblock {
    uint64_t generation;
    uint64_t payloadOffset;
    uint64_t payloadSize;
    uint64_t payloadChecksum;
  };

  struct LoadedGeneration {
    Superblock superblock;
    std::shared_ptr<ProfileNamespaceNode> root;
    uint64_t nextNodeID;
  };

  struct BootstrapJournalRecord {
    uint64_t generation;
    ProfileNamespaceBootstrapState state;
  };

  std::recursive_mutex namespaceMutex;
  std::shared_ptr<OPFSDirectory> physicalRoot;
  std::shared_ptr<OPFSFile> journal;
  std::shared_ptr<OPFSFile> container;
  std::shared_ptr<ProfileNamespaceNode> root;
  uint64_t nextNodeID = 1;
  uint64_t generation = 0;
  uint64_t appendOffset = kProfileNamespacePayloadOffset;
  uint64_t profileNameChecksum = 0;
  size_t profileNameLength = 0;
  uint64_t journalGeneration = 0;
  ProfileNamespaceBootstrapState bootstrapState =
    ProfileNamespaceBootstrapState::Prepared;
  // A root is exposed only after both alternating journal records durably say
  // PUBLISHED. A single PUBLISHED record next to the old PREPARED record is
  // an interrupted mirror that a fresh factory may complete, but never an
  // established profile. A lone surviving PUBLISHED record is fail-closed.
  bool journalPublicationMirrored = false;
  bool journalOpen = false;
  bool containerOpen = false;
  // A known failure can occur after creating a journal directory entry but
  // before its PREPARED record was durably acknowledged. That unpublishable
  // object is the only factory-time state eligible for local cleanup.
  bool discardUnpublishedJournal = false;
  // A ProxyWorker callback which does not acknowledge completion leaves its
  // browser-side effect unknown.  In particular, a cancelled insert can have
  // created the opaque container even though native code never received its
  // handle ID.  Such a factory failure must retain the lease-owner
  // reservation and the dedicated worker until document teardown; attempting
  // a later cleanup or Web Locks release would be a false handoff.
  bool initialisationAmbiguous = false;
  // WasmFS File objects have one parent and own their record-lock and append
  // serialization state. Do not manufacture a second alias mount with fresh
  // wrapper identity for the same logical nodes; a caller that needs to
  // remount must complete the result-bearing drain and create a fresh backend.
  bool rootExposed = false;
  std::string containerName;
  std::string journalName;
  int fatalError = 0;

  static uint64_t checksum(const uint8_t* data, size_t size) {
    // The checksum is a corruption detector, not a cryptographic integrity
    // primitive.  The cooperative profile lease remains the writer-admission
    // mechanism for this container.
    uint64_t result = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < size; ++i) {
      result ^= data[i];
      result *= UINT64_C(1099511628211);
    }
    return result;
  }

  static std::string namespaceStorageStem(std::string_view profileName) {
    // The profile component is length-delimited rather than suffix-delimited.
    // In particular, profiles "collision" and "collision.bootstrap" must
    // never map one profile's journal/container name onto the other profile's
    // published name. IsValidProfileLeaseName bounds the component to 128
    // ASCII bytes, keeping the resulting OPFS names comfortably bounded.
    std::string result = ".wasmfs-profile-namespace-v1-";
    result += std::to_string(profileName.size());
    result += '-';
    result.append(profileName.data(), profileName.size());
    return result;
  }

  static void appendU32(std::vector<uint8_t>& data, uint32_t value) {
    for (size_t i = 0; i != sizeof(value); ++i) {
      data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
  }

  static void appendU64(std::vector<uint8_t>& data, uint64_t value) {
    for (size_t i = 0; i != sizeof(value); ++i) {
      data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
  }

  static bool readU32(const uint8_t* data,
                      size_t size,
                      size_t* cursor,
                      uint32_t* value) {
    if (*cursor > size || size - *cursor < sizeof(*value)) {
      return false;
    }
    *value = 0;
    for (size_t i = 0; i != sizeof(*value); ++i) {
      *value |= uint32_t(data[*cursor + i]) << (8 * i);
    }
    *cursor += sizeof(*value);
    return true;
  }

  static bool readU64(const uint8_t* data,
                      size_t size,
                      size_t* cursor,
                      uint64_t* value) {
    if (*cursor > size || size - *cursor < sizeof(*value)) {
      return false;
    }
    *value = 0;
    for (size_t i = 0; i != sizeof(*value); ++i) {
      *value |= uint64_t(data[*cursor + i]) << (8 * i);
    }
    *cursor += sizeof(*value);
    return true;
  }

  static bool validEntryName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.size() <= kProfileNamespaceNameMax &&
           name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos;
  }

  static bool serialiseNode(const std::shared_ptr<ProfileNamespaceNode>& node,
                            std::vector<uint8_t>* output,
                            std::unordered_set<uint64_t>* seen,
                            size_t depth) {
    constexpr size_t kNodeHeaderSize =
      sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t);
    if (!node || !node->id || depth > kMaxProfileNamespaceDepth ||
        seen->size() >= kMaxProfileNamespaceNodes ||
        output->size() > kMaxProfileNamespacePayloadBytes ||
        kNodeHeaderSize > kMaxProfileNamespacePayloadBytes - output->size() ||
        !seen->insert(node->id).second) {
      return false;
    }

    output->push_back(static_cast<uint8_t>(node->kind));
    appendU64(*output, node->id);
    appendU32(*output, node->mode & (S_IRWXUGO | S_ISVTX));
    if (node->kind == ProfileNamespaceNodeKind::File) {
      if (sizeof(uint64_t) >
            kMaxProfileNamespacePayloadBytes - output->size() ||
          node->data.size() > kMaxProfileNamespacePayloadBytes -
            output->size() - sizeof(uint64_t)) {
        return false;
      }
      appendU64(*output, node->data.size());
      output->insert(output->end(), node->data.begin(), node->data.end());
      return true;
    }

    if (node->kind != ProfileNamespaceNodeKind::Directory ||
        node->children.size() > kMaxProfileNamespaceNodes ||
        sizeof(uint32_t) >
          kMaxProfileNamespacePayloadBytes - output->size()) {
      return false;
    }
    appendU32(*output, node->children.size());
    for (const auto& [name, child] : node->children) {
      if (!validEntryName(name) ||
          name.size() > std::numeric_limits<uint32_t>::max() ||
          sizeof(uint32_t) >
            kMaxProfileNamespacePayloadBytes - output->size() ||
          name.size() > kMaxProfileNamespacePayloadBytes - output->size() -
            sizeof(uint32_t)) {
        return false;
      }
      appendU32(*output, name.size());
      output->insert(output->end(), name.begin(), name.end());
      if (!serialiseNode(child, output, seen, depth + 1)) {
        return false;
      }
    }
    return true;
  }

  static std::shared_ptr<ProfileNamespaceNode> deserialiseNode(
    const uint8_t* data,
    size_t size,
    size_t* cursor,
    const std::shared_ptr<ProfileNamespaceNode>& parent,
    std::unordered_set<uint64_t>* seen,
    size_t* nodeCount,
    uint64_t* maxNodeID,
    size_t depth) {
    if (*cursor >= size || *nodeCount >= kMaxProfileNamespaceNodes ||
        depth > kMaxProfileNamespaceDepth) {
      return nullptr;
    }
    auto rawKind = data[(*cursor)++];
    ProfileNamespaceNodeKind kind;
    if (rawKind == static_cast<uint8_t>(ProfileNamespaceNodeKind::File)) {
      kind = ProfileNamespaceNodeKind::File;
    } else if (rawKind ==
               static_cast<uint8_t>(ProfileNamespaceNodeKind::Directory)) {
      kind = ProfileNamespaceNodeKind::Directory;
    } else {
      return nullptr;
    }

    uint64_t id;
    uint32_t mode;
    if (!readU64(data, size, cursor, &id) || !id ||
        !seen->insert(id).second || !readU32(data, size, cursor, &mode)) {
      return nullptr;
    }
    auto node = std::make_shared<ProfileNamespaceNode>(
      kind, id, mode & (S_IRWXUGO | S_ISVTX));
    node->parent = parent;
    ++*nodeCount;
    *maxNodeID = std::max(*maxNodeID, id);

    if (kind == ProfileNamespaceNodeKind::File) {
      uint64_t dataSize;
      if (!readU64(data, size, cursor, &dataSize) ||
          dataSize > kMaxProfileNamespacePayloadBytes ||
          dataSize > size - *cursor) {
        return nullptr;
      }
      node->data.insert(
        node->data.end(), data + *cursor, data + *cursor + dataSize);
      *cursor += dataSize;
      return node;
    }

    uint32_t childCount;
    if (!readU32(data, size, cursor, &childCount) ||
        childCount > kMaxProfileNamespaceNodes - *nodeCount) {
      return nullptr;
    }
    for (uint32_t i = 0; i != childCount; ++i) {
      uint32_t nameSize;
      if (!readU32(data, size, cursor, &nameSize) ||
          nameSize > kProfileNamespaceNameMax || nameSize > size - *cursor) {
        return nullptr;
      }
      std::string name(reinterpret_cast<const char*>(data + *cursor),
                       nameSize);
      *cursor += nameSize;
      if (!validEntryName(name)) {
        return nullptr;
      }
      auto child = deserialiseNode(
        data, size, cursor, node, seen, nodeCount, maxNodeID, depth + 1);
      if (!child || !node->children.emplace(std::move(name), child).second) {
        return nullptr;
      }
    }
    return node;
  }

  static bool serialiseTree(const std::shared_ptr<ProfileNamespaceNode>& root,
                            std::vector<uint8_t>* output) {
    output->clear();
    output->insert(output->end(),
                   kProfileNamespacePayloadMagic.begin(),
                   kProfileNamespacePayloadMagic.end());
    appendU32(*output, kProfileNamespaceFormatVersion);
    std::unordered_set<uint64_t> seen;
    return serialiseNode(root, output, &seen, 0) &&
           output->size() <= kMaxProfileNamespacePayloadBytes;
  }

  static std::optional<std::pair<std::shared_ptr<ProfileNamespaceNode>,
                                 uint64_t>>
  deserialiseTree(const std::vector<uint8_t>& input) {
    if (input.size() < kProfileNamespacePayloadMagic.size() +
                         sizeof(uint32_t) ||
        !std::equal(kProfileNamespacePayloadMagic.begin(),
                    kProfileNamespacePayloadMagic.end(),
                    input.begin())) {
      return std::nullopt;
    }
    size_t cursor = kProfileNamespacePayloadMagic.size();
    uint32_t version;
    if (!readU32(input.data(), input.size(), &cursor, &version) ||
        version != kProfileNamespaceFormatVersion) {
      return std::nullopt;
    }
    std::unordered_set<uint64_t> seen;
    size_t nodeCount = 0;
    uint64_t maxNodeID = 0;
    auto parsed = deserialiseNode(input.data(),
                                  input.size(),
                                  &cursor,
                                  nullptr,
                                  &seen,
                                  &nodeCount,
                                  &maxNodeID,
                                  0);
    if (!parsed || parsed->kind != ProfileNamespaceNodeKind::Directory ||
        cursor != input.size() ||
        maxNodeID == std::numeric_limits<uint64_t>::max()) {
      return std::nullopt;
    }
    return std::make_pair(parsed, maxNodeID + 1);
  }

  template <size_t Size>
  static void writeHeaderU32(std::array<uint8_t, Size>& data,
                             size_t offset,
                             uint32_t value) {
    for (size_t i = 0; i != sizeof(value); ++i) {
      data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
  }

  template <size_t Size>
  static void writeHeaderU64(std::array<uint8_t, Size>& data,
                             size_t offset,
                             uint64_t value) {
    for (size_t i = 0; i != sizeof(value); ++i) {
      data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
  }

  template <size_t Size>
  static uint32_t readHeaderU32(const std::array<uint8_t, Size>& data,
                                size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i != sizeof(value); ++i) {
      value |= uint32_t(data[offset + i]) << (8 * i);
    }
    return value;
  }

  template <size_t Size>
  static uint64_t readHeaderU64(const std::array<uint8_t, Size>& data,
                                size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i != sizeof(value); ++i) {
      value |= uint64_t(data[offset + i]) << (8 * i);
    }
    return value;
  }

  static std::array<uint8_t, kProfileNamespaceSuperblockSize> makeSuperblock(
    const Superblock& superblock) {
    std::array<uint8_t, kProfileNamespaceSuperblockSize> data = {};
    std::copy(kProfileNamespaceSuperblockMagic.begin(),
              kProfileNamespaceSuperblockMagic.end(),
              data.begin());
    writeHeaderU32(data, 8, kProfileNamespaceFormatVersion);
    writeHeaderU32(data, 12, kProfileNamespaceSuperblockSize);
    writeHeaderU64(data, 16, superblock.generation);
    writeHeaderU64(data, 24, superblock.payloadOffset);
    writeHeaderU64(data, 32, superblock.payloadSize);
    writeHeaderU64(data, 40, superblock.payloadChecksum);
    writeHeaderU64(data, 48, checksum(data.data(), 48));
    return data;
  }

  static std::optional<Superblock> parseSuperblock(
    const std::array<uint8_t, kProfileNamespaceSuperblockSize>& data) {
    if (!std::equal(kProfileNamespaceSuperblockMagic.begin(),
                    kProfileNamespaceSuperblockMagic.end(),
                    data.begin()) ||
        readHeaderU32(data, 8) != kProfileNamespaceFormatVersion ||
        readHeaderU32(data, 12) != kProfileNamespaceSuperblockSize ||
        readHeaderU64(data, 48) != checksum(data.data(), 48)) {
      return std::nullopt;
    }
    Superblock result = {readHeaderU64(data, 16),
                         readHeaderU64(data, 24),
                         readHeaderU64(data, 32),
                         readHeaderU64(data, 40)};
    if (!result.generation || !result.payloadSize ||
        result.payloadSize > kMaxProfileNamespacePayloadBytes ||
        result.payloadOffset < kProfileNamespacePayloadOffset ||
        result.payloadOffset > std::numeric_limits<uint64_t>::max() -
          result.payloadSize) {
      return std::nullopt;
    }
    return result;
  }

  std::array<uint8_t, kProfileNamespaceJournalRecordSize> makeJournalRecord(
    const BootstrapJournalRecord& record) const {
    std::array<uint8_t, kProfileNamespaceJournalRecordSize> data = {};
    std::copy(kProfileNamespaceJournalMagic.begin(),
              kProfileNamespaceJournalMagic.end(),
              data.begin());
    writeHeaderU32(data, 8, kProfileNamespaceJournalFormatVersion);
    writeHeaderU32(data, 12, kProfileNamespaceJournalRecordSize);
    writeHeaderU64(data, 16, record.generation);
    writeHeaderU32(data, 24, static_cast<uint32_t>(record.state));
    writeHeaderU32(data, 28, 0);
    writeHeaderU64(data, 32, profileNameChecksum);
    writeHeaderU32(data, 40, static_cast<uint32_t>(profileNameLength));
    writeHeaderU32(data, 44, 0);
    writeHeaderU64(data, 48, checksum(data.data(), 48));
    return data;
  }

  std::optional<BootstrapJournalRecord> parseJournalRecord(
    const std::array<uint8_t, kProfileNamespaceJournalRecordSize>& data) const {
    if (!std::equal(kProfileNamespaceJournalMagic.begin(),
                    kProfileNamespaceJournalMagic.end(),
                    data.begin()) ||
        readHeaderU32(data, 8) != kProfileNamespaceJournalFormatVersion ||
        readHeaderU32(data, 12) != kProfileNamespaceJournalRecordSize ||
        readHeaderU64(data, 48) != checksum(data.data(), 48) ||
        readHeaderU64(data, 32) != profileNameChecksum ||
        readHeaderU32(data, 40) != profileNameLength) {
      return std::nullopt;
    }
    const uint32_t rawState = readHeaderU32(data, 24);
    ProfileNamespaceBootstrapState state;
    if (rawState == static_cast<uint32_t>(
                      ProfileNamespaceBootstrapState::Prepared)) {
      state = ProfileNamespaceBootstrapState::Prepared;
    } else if (rawState == static_cast<uint32_t>(
                             ProfileNamespaceBootstrapState::Published)) {
      state = ProfileNamespaceBootstrapState::Published;
    } else {
      return std::nullopt;
    }
    const uint64_t generation = readHeaderU64(data, 16);
    if (!generation) {
      return std::nullopt;
    }
    return BootstrapJournalRecord{generation, state};
  }

  int poisonLocked(int error) {
    if (error >= 0) {
      error = -EIO;
    }
    if (!fatalError) {
      fatalError = error;
    }
    return fatalError;
  }

  int writeContainerLocked(uint64_t offset, const uint8_t* data, size_t size) {
    if (!containerOpen || offset > std::numeric_limits<off_t>::max()) {
      return -ESHUTDOWN;
    }
    auto result = container->locked().write(data, size, offset);
    if (result < 0) {
      return result;
    }
    return size_t(result) == size ? 0 : -EIO;
  }

  int readContainerLocked(uint64_t offset, uint8_t* data, size_t size) {
    if (!containerOpen || offset > std::numeric_limits<off_t>::max()) {
      return -ESHUTDOWN;
    }
    auto result = container->locked().read(data, size, offset);
    if (result < 0) {
      return result;
    }
    return size_t(result) == size ? 0 : -EIO;
  }

  int resizeContainerLocked(uint64_t size) {
    if (!containerOpen || size > std::numeric_limits<off_t>::max()) {
      return -ESHUTDOWN;
    }
    return container->locked().setSize(size);
  }

  int flushContainerLocked() {
    if (!containerOpen) {
      return -ESHUTDOWN;
    }
    return container->locked().flush();
  }

  int commitLocked() {
    if (fatalError) {
      return fatalError;
    }
    std::vector<uint8_t> payload;
    if (!serialiseTree(root, &payload)) {
      return poisonLocked(-EFBIG);
    }
    if (generation == std::numeric_limits<uint64_t>::max() ||
        appendOffset > std::numeric_limits<uint64_t>::max() - payload.size()) {
      return poisonLocked(-EOVERFLOW);
    }
    uint64_t payloadEnd = appendOffset + payload.size();
    if (payloadEnd > kMaxProfileNamespaceContainerBytes) {
      return poisonLocked(-ENOSPC);
    }
    if (int error = resizeContainerLocked(payloadEnd)) {
      return poisonLocked(error);
    }
    if (int error = writeContainerLocked(
          appendOffset, payload.data(), payload.size())) {
      return poisonLocked(error);
    }
    if (int error = flushContainerLocked()) {
      return poisonLocked(error);
    }

    Superblock next = {generation + 1,
                       appendOffset,
                       payload.size(),
                       checksum(payload.data(), payload.size())};
    auto selector = makeSuperblock(next);
#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
    wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(1);
#endif
    uint64_t selectorOffset =
      (next.generation & 1) * kProfileNamespaceSuperblockSize;
    if (int error = writeContainerLocked(
          selectorOffset, selector.data(), selector.size())) {
      return poisonLocked(error);
    }
    if (int error = flushContainerLocked()) {
      return poisonLocked(error);
    }
#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
    wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(2);
#endif
    generation = next.generation;
    appendOffset = payloadEnd;
    return 0;
  }

  // Returns a completed OPFS error separately from an invalid namespace
  // image. Recovery may reset only an explicitly PREPARED, structurally
  // invalid container; it must never turn a short read or completed I/O error
  // into data deletion.
  int loadLocked(uint64_t size, bool* structurallyValid) {
    *structurallyValid = false;
    if (size > kMaxProfileNamespaceContainerBytes ||
        size < kProfileNamespacePayloadOffset) {
      return 0;
    }
    std::optional<LoadedGeneration> selected;
    for (uint64_t selector = 0; selector != 2; ++selector) {
      std::array<uint8_t, kProfileNamespaceSuperblockSize> header;
      if (int error = readContainerLocked(
            selector * kProfileNamespaceSuperblockSize,
            header.data(),
            header.size())) {
        return error;
      }
      auto superblock = parseSuperblock(header);
      // Each generation is published only to its alternating selector.  Do
      // not accept a complete-but-misplaced record as a substitute for the
      // selected generation: that would allow a corrupted slot to resurrect
      // a namespace image that the commit protocol never made current.
      if (!superblock || (superblock->generation & 1) != selector ||
          superblock->payloadOffset > size ||
          superblock->payloadSize > size - superblock->payloadOffset) {
        continue;
      }
      std::vector<uint8_t> payload(superblock->payloadSize);
      if (int error = readContainerLocked(
            superblock->payloadOffset, payload.data(), payload.size())) {
        return error;
      }
      if (checksum(payload.data(), payload.size()) !=
          superblock->payloadChecksum) {
        continue;
      }
      auto parsed = deserialiseTree(payload);
      if (!parsed) {
        continue;
      }
      if (selected &&
          superblock->generation == selected->superblock.generation) {
        // This should be unreachable with the selector-parity rule above,
        // but preserve an explicit fail-closed check if that invariant ever
        // changes.  Two independently valid records cannot both be the one
        // committed generation.
        return 0;
      }
      if (!selected ||
          superblock->generation > selected->superblock.generation) {
        selected = LoadedGeneration{
          *superblock, std::move(parsed->first), parsed->second};
      }
    }
    if (!selected) {
      return 0;
    }
    root = std::move(selected->root);
    nextNodeID = selected->nextNodeID;
    generation = selected->superblock.generation;
    appendOffset = size;
    *structurallyValid = true;
    return 0;
  }

  int closeContainerForRetirementLocked(bool flushFirst) {
    if (!containerOpen) {
      return 0;
    }
    int firstError = 0;
    if (flushFirst) {
      firstError = flushContainerLocked();
    }
    int closeError = container->locked().close();
    if (!closeError) {
      containerOpen = false;
    } else if (!firstError) {
      firstError = closeError;
    }
    return firstError;
  }

  int recordInitialisationProxyFailure() {
    initialisationAmbiguous = true;
    // A raw directory operation has no OPFSFile wrapper that can otherwise
    // carry the lost-completion state into scoped retirement. Latch it in the
    // backend-wide terminal state so a later drain cannot release a lease
    // while create/remove ownership is unknown.
    terminalCloseState->recordFailedAccessClose(-EIO);
    return -EIO;
  }

  std::shared_ptr<OPFSFile> makePhysicalFile(const std::string& name) {
    auto file = std::make_shared<OPFSFile>(
      0600,
      this,
      kOPFSRootDirectoryID,
      name,
      proxy,
      terminalCloseState,
      profileLeaseState);
    file->locked().setParent(physicalRoot);
    return file;
  }

  void makeJournal() { journal = makePhysicalFile(journalName); }

  void makeContainer() { container = makePhysicalFile(containerName); }

  int lookupPhysicalChildLocked(const std::string& name,
                                int* childType,
                                int* childID) {
    *childType = 0;
    *childID = -EIO;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_get_child(ctx.ctx,
                                 kOPFSRootDirectoryID,
                                 name.c_str(),
                                 childType,
                                 childID);
        })) {
      return recordInitialisationProxyFailure();
    }
    return 0;
  }

  int insertPhysicalFileLocked(const std::string& name, bool testJournal) {
    int childID = -EIO;
    bool completed = proxy([&](auto ctx) {
      _wasmfs_opfs_insert_file(
        ctx.ctx, kOPFSRootDirectoryID, name.c_str(), &childID);
    });
#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE == 3
    if (testJournal && completed) {
      return recordInitialisationProxyFailure();
    }
#endif
    if (!completed) {
      return recordInitialisationProxyFailure();
    }
    if (childID < 0) {
      return childID;
    }
    return childID == 0 ? 0 : -EIO;
  }

  int removePhysicalChildLocked(const std::string& name) {
    int error = 0;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_remove_child(
            ctx.ctx, kOPFSRootDirectoryID, name.c_str(), &error);
        })) {
      // The browser may have completed the remove even though the native side
      // did not see its acknowledgement. Do not issue another cleanup or
      // lease-release transaction from this factory instance.
      return recordInitialisationProxyFailure();
    }
    return error;
  }

  int openJournalLocked() {
    if (!journal) {
      return -EIO;
    }
    if (int error = journal->locked().open(O_RDWR)) {
      return error;
    }
    journalOpen = true;
    return 0;
  }

  int closeJournalForRetirementLocked(bool flushFirst) {
    if (!journalOpen) {
      return 0;
    }
    int firstError = flushFirst ? journal->locked().flush() : 0;
    int closeError = journal->locked().close();
    if (!closeError) {
      journalOpen = false;
    } else if (!firstError) {
      firstError = closeError;
    }
    return firstError;
  }

  int writeJournalRecordLocked(ProfileNamespaceBootstrapState state) {
    if (!journalOpen || journalGeneration ==
                         std::numeric_limits<uint64_t>::max()) {
      return journalOpen ? -EOVERFLOW : -ESHUTDOWN;
    }
    const BootstrapJournalRecord next = {journalGeneration + 1, state};
    const auto record = makeJournalRecord(next);
    if (int error = journal->locked().setSize(kProfileNamespaceJournalSize)) {
      return error;
    }
    const uint64_t offset =
      (next.generation & 1) * kProfileNamespaceJournalRecordSize;
    const ssize_t written = journal->locked().write(
      record.data(), record.size(), offset);
    if (written < 0) {
      return written;
    }
    if (size_t(written) != record.size()) {
      return -EIO;
    }
    if (int error = journal->locked().flush()) {
      return error;
    }
    journalGeneration = next.generation;
    bootstrapState = next.state;
    discardUnpublishedJournal = false;
    return 0;
  }

  int finishPublishedJournalLocked() {
    if (!journalOpen ||
        bootstrapState != ProfileNamespaceBootstrapState::Published ||
        journalPublicationMirrored) {
      return -ESHUTDOWN;
    }
    if (int error = writeJournalRecordLocked(
          ProfileNamespaceBootstrapState::Published)) {
      return error;
    }
    journalPublicationMirrored = true;
#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
    // This is intentionally after the quorum completes but before the first
    // createDirectory() caller exposes the logical root.
    wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(5);
#endif
    return 0;
  }

  int writeJournalLocked(ProfileNamespaceBootstrapState state) {
    if (state == ProfileNamespaceBootstrapState::Prepared) {
      journalPublicationMirrored = false;
      if (int error = writeJournalRecordLocked(state)) {
        return error;
      }
#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
      wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(3);
#endif
      return 0;
    }

    // The first PUBLISHED record is a durable publication decision, but it
    // does not authorize root exposure. Keep PREPARED in the other slot until
    // a second PUBLISHED record has flushed; recovery can then distinguish a
    // torn first-mirror from a corrupted established profile.
    journalPublicationMirrored = false;
    if (int error = writeJournalRecordLocked(state)) {
      return error;
    }
#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
    wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(
      4);
#endif
    return finishPublishedJournalLocked();
  }

  // A false |structurallyValid| result means both slots were read without an
  // operational error but contained no usable record. Callers may reset it
  // only when no canonical profile exists. A negative return always means a
  // completed (or terminal-latched) browser operation failure.
  int loadJournalLocked(uint64_t size, bool* structurallyValid) {
    *structurallyValid = false;
    journalGeneration = 0;
    bootstrapState = ProfileNamespaceBootstrapState::Prepared;
    journalPublicationMirrored = false;
    if (size != kProfileNamespaceJournalSize) {
      return 0;
    }
    std::array<std::optional<BootstrapJournalRecord>, 2> records;
    for (uint64_t slot = 0; slot != 2; ++slot) {
      std::array<uint8_t, kProfileNamespaceJournalRecordSize> data;
      const ssize_t read = journal->locked().read(
        data.data(), data.size(), slot * kProfileNamespaceJournalRecordSize);
      if (read < 0) {
        return read;
      }
      if (size_t(read) != data.size()) {
        return -EIO;
      }
      auto record = parseJournalRecord(data);
      if (!record || (record->generation & 1) != slot) {
        continue;
      }
      records[slot] = *record;
    }

#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION == 1
    // Exercise the same parser state as a damaged/lost slot without granting
    // the test binary host-OPFS write access. This is deliberately after a
    // successful physical read: the only assertion is that a completed
    // PUBLISHED quorum never falls back to reset when either witness is gone.
    records[0].reset();
#elif WASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION == 2
    records[1].reset();
#endif

    const bool hasFirst = records[0].has_value();
    const bool hasSecond = records[1].has_value();
    if (!hasFirst && !hasSecond) {
      return 0;
    }

    if (hasFirst != hasSecond) {
      const BootstrapJournalRecord& record =
        hasFirst ? *records[0] : *records[1];
      // A completed PUBLISHED record has no safe single-record recovery
      // interpretation: it may be the sole survivor of a corrupt completed
      // mirror. Only a lone PREPARED record can authorize a new first mount.
      if (record.generation == 1 &&
          record.state == ProfileNamespaceBootstrapState::Prepared) {
        journalGeneration = record.generation;
        bootstrapState = record.state;
        *structurallyValid = true;
        return 0;
      }
      if (record.state == ProfileNamespaceBootstrapState::Published) {
        return -EIO;
      }
      return 0;
    }

    const BootstrapJournalRecord& first = *records[0];
    const BootstrapJournalRecord& second = *records[1];
    if (first.generation == 2 &&
        first.state == ProfileNamespaceBootstrapState::Published &&
        second.generation == 1 &&
        second.state == ProfileNamespaceBootstrapState::Prepared) {
      // The first PUBLISHED copy flushed, but its mirror did not. The root is
      // still pre-exposure and must be validated as empty before a caller may
      // finish the mirror in createDirectory().
      journalGeneration = first.generation;
      bootstrapState = first.state;
      *structurallyValid = true;
      return 0;
    }
    if (first.generation == 2 && second.generation == 3 &&
        first.state == ProfileNamespaceBootstrapState::Published &&
        second.state == ProfileNamespaceBootstrapState::Published) {
      journalGeneration = second.generation;
      bootstrapState = second.state;
      journalPublicationMirrored = true;
      *structurallyValid = true;
      return 0;
    }

    // Version 1 never advances the bootstrap journal after g3. Treat every
    // other well-formed sequence containing PUBLISHED as an integrity error,
    // even without a canonical container: deleting it could transform a
    // damaged established profile into a fresh empty namespace.
    if (first.state == ProfileNamespaceBootstrapState::Published ||
        second.state == ProfileNamespaceBootstrapState::Published) {
      return -EIO;
    }
    return 0;
  }

  int openContainerLocked() {
    if (!container) {
      return -EIO;
    }
    if (int error = container->locked().open(O_RDWR)) {
      return error;
    }
    containerOpen = true;
    return 0;
  }

  int loadExistingContainerLocked(bool* structurallyValid) {
    if (!containerOpen) {
      return -ESHUTDOWN;
    }
    const off_t physicalSize = container->locked().getSize();
    if (physicalSize < 0) {
      return physicalSize;
    }
    return loadLocked(physicalSize, structurallyValid);
  }

  int discardPreparedContainerLocked() {
    if (containerOpen) {
      if (int error = closeContainerForRetirementLocked(false)) {
        return error;
      }
    }
    if (int error = removePhysicalChildLocked(containerName)) {
      return error;
    }
    container.reset();
    root.reset();
    nextNodeID = 1;
    generation = 0;
    appendOffset = kProfileNamespacePayloadOffset;
    return 0;
  }

  int createPreparedJournalLocked() {
    if (int error = insertPhysicalFileLocked(journalName, true)) {
      return error;
    }
    // A local known failure after the acknowledged directory insertion can
    // remove this object because no PREPARED contents have been acknowledged
    // and no canonical namespace was allowed to exist after it.
    discardUnpublishedJournal = true;
    makeJournal();
#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE == 4
    return -EIO;
#endif
    if (int error = openJournalLocked()) {
      return error;
    }
    return writeJournalLocked(ProfileNamespaceBootstrapState::Prepared);
  }

  std::shared_ptr<File> makeFile(
    const std::shared_ptr<ProfileNamespaceNode>& node);
  std::shared_ptr<DataFile> makeDataFile(
    const std::shared_ptr<ProfileNamespaceNode>& node);
  std::shared_ptr<Directory> makeDirectory(
    const std::shared_ptr<ProfileNamespaceNode>& node);

public:
  bool supportsExplicitMetadataMutation() const override { return false; }

  int initialise(const char* profileName) {
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    bool rootInitialised = proxy(
      [](auto ctx) { _wasmfs_opfs_init_root_directory(ctx.ctx); });
#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE == 1
    // The focused factory test has observed this callback but deliberately
    // discards its completion to exercise the same native ownership outcome
    // as a lost ProxyWorker acknowledgement.
    if (rootInitialised) {
      return recordInitialisationProxyFailure();
    }
#endif
    if (!rootInitialised) {
      return recordInitialisationProxyFailure();
    }
    physicalRoot = std::make_shared<OPFSDirectory>(
      0700,
      this,
      kOPFSRootDirectoryID,
      proxy,
      terminalCloseState,
      profileLeaseState);
    const std::string_view profileNameView(profileName);
    profileNameLength = profileNameView.size();
    profileNameChecksum = checksum(
      reinterpret_cast<const uint8_t*>(profileNameView.data()),
      profileNameLength);
    const std::string nameStem = namespaceStorageStem(profileNameView);
    containerName = nameStem + ".container";
    journalName = nameStem + ".journal";

    int containerType = 0;
    int containerID = -EIO;
    const bool containerLookupCompleted = proxy([&](auto ctx) {
      _wasmfs_opfs_get_child(ctx.ctx,
                             kOPFSRootDirectoryID,
                             containerName.c_str(),
                             &containerType,
                             &containerID);
    });
#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE == 2
    if (containerLookupCompleted) {
      return recordInitialisationProxyFailure();
    }
#endif
    if (!containerLookupCompleted) {
      return recordInitialisationProxyFailure();
    }
    if (containerID < 0 && containerID != -ENOENT) {
      return containerID;
    }

    for (;;) {
      int journalType = 0;
      int journalID = -EIO;
      if (int error =
            lookupPhysicalChildLocked(journalName, &journalType, &journalID)) {
        return error;
      }
      if (journalID == -ENOENT) {
        // No journal means there has never been a durably acknowledged
        // publication decision. A pre-existing canonical file is therefore
        // fail-closed rather than silently adopted or reset.
        if (containerID != -ENOENT) {
          return -EIO;
        }
        return createPreparedJournalLocked();
      }
      if (journalID < 0 || journalType != 1 || journalID != 0) {
        return journalID < 0 ? journalID : -EIO;
      }

      makeJournal();
      if (int error = openJournalLocked()) {
        return error;
      }
      const off_t journalSize = journal->locked().getSize();
      if (journalSize < 0) {
        return journalSize;
      }
      bool journalValid = false;
      if (int error = loadJournalLocked(journalSize, &journalValid)) {
        return error;
      }
      if (!journalValid) {
        // A malformed journal can be reset only while no canonical name
        // exists. Once a canonical object exists, missing/corrupt journal
        // intent is an integrity failure rather than a profile reset.
        if (containerID != -ENOENT) {
          return -EIO;
        }
        if (int error = closeJournalForRetirementLocked(false)) {
          return error;
        }
        if (int error = removePhysicalChildLocked(journalName)) {
          return error;
        }
        journal.reset();
        journalGeneration = 0;
        continue;
      }

      if (bootstrapState == ProfileNamespaceBootstrapState::Published) {
        if (containerID == -ENOENT || containerType != 1 ||
            containerID != 0) {
          return -EIO;
        }
        makeContainer();
        if (int error = openContainerLocked()) {
          return error;
        }
        bool containerValid = false;
        if (int error = loadExistingContainerLocked(&containerValid)) {
          return error;
        }
        if (!containerValid) {
          return -EIO;
        }
        if (!journalPublicationMirrored &&
            (!root || root->kind != ProfileNamespaceNodeKind::Directory ||
             !root->children.empty())) {
          // PREPARED(g1)+PUBLISHED(g2) is a torn pre-exposure mirror, not a
          // partial established profile. It can contain only the initial
          // empty root before a fresh createDirectory() finishes g3.
          return -EIO;
        }
        return 0;
      }

      // PREPARED authorizes recovery only of the not-yet-exposed initial root.
      // A missing container is normal before the first mount. A structurally
      // invalid file is safe to discard only because the durable journal has
      // not advanced to PUBLISHED. Completed I/O failures always propagate.
      if (containerID == -ENOENT) {
        return 0;
      }
      if (containerType != 1 || containerID != 0) {
        return -EIO;
      }
      makeContainer();
      if (int error = openContainerLocked()) {
        return error;
      }
      bool containerValid = false;
      if (int error = loadExistingContainerLocked(&containerValid)) {
        return error;
      }
      if (!containerValid) {
        if (int error = discardPreparedContainerLocked()) {
          return error;
        }
        return 0;
      }
      // Before PUBLISHED the root has not been returned to a caller, so a
      // recovery retry may set its requested mount mode. Any populated tree
      // here cannot be explained by this protocol and must not be adopted.
      if (!root || root->kind != ProfileNamespaceNodeKind::Directory ||
          !root->children.empty()) {
        return -EIO;
      }
      return 0;
    }
  }

  // A factory failure occurs before the backend is owned by WasmFS.  Make the
  // same explicit handoff attempt used by an ordinary profile drain; if any
  // step is ambiguous, retain the lease/worker tombstone rather than making a
  // later module believe it owns the container.
  bool abandonFailedInitialisation() {
    if (initialisationAmbiguous || getOPFSProfilePriorCloseError()) {
      // Do not use either namespace retirement override or the inherited
      // browser-resource preflight here. An unacknowledged initialization
      // callback can have opened an access handle or inserted the container,
      // and any cleanup/release proxy would be a second ambiguous operation.
      // Close only the native destructor admission gate, seal the lease state,
      // and retain its worker/reservation tombstone until document teardown.
      profileLeaseState->closeDestructorProxying();
      if (beginOPFSProfileDrain() == 0) {
        bool leaseReleased = false;
        (void)finishOPFSProfileDrain(false, &leaseReleased);
        assert(!leaseReleased);
      } else {
        proxy.abandonScopedProfileWorker();
      }
      return false;
    }
    if (beginOPFSProfileDrain()) {
      return false;
    }
    int preparation = prepareOPFSProfileRetirement(true);
    bool leaseReleased = false;
    int finishError = finishOPFSProfileDrain(
      preparation == 0, &leaseReleased);
    if (leaseReleased) {
      // Once the browser has acknowledged the release, retirement is still
      // mandatory to prevent a later global destructor from joining on the
      // browser main thread.  Do it even after an earlier cleanup error, but
      // only report a factory handoff as complete when every stage succeeded.
      int retirement = retireOPFSProfileBackend(
        preparation == 0 && finishError == 0);
      return preparation == 0 && finishError == 0 && retirement == 0;
    }
    return false;
  }

  off_t getFileSize(const std::shared_ptr<ProfileNamespaceNode>& node) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!node || node->kind != ProfileNamespaceNodeKind::File) {
      return -EIO;
    }
    if (uint64_t(node->data.size()) >
        uint64_t(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    return node->data.size();
  }

  int openFile(const std::shared_ptr<ProfileNamespaceNode>& node,
               oflags_t flags) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!node || node->kind != ProfileNamespaceNodeKind::File ||
        (flags != O_RDONLY && flags != O_WRONLY && flags != O_RDWR)) {
      return -EIO;
    }
    return 0;
  }

  int closeFile(const std::shared_ptr<ProfileNamespaceNode>& node) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (!node || node->kind != ProfileNamespaceNodeKind::File) {
      return -EIO;
    }
    return fatalError;
  }

  ssize_t readFile(const std::shared_ptr<ProfileNamespaceNode>& node,
                   uint8_t* buffer,
                   size_t length,
                   off_t offset) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!node || node->kind != ProfileNamespaceNodeKind::File || offset < 0) {
      return -EINVAL;
    }
    if (length == 0) {
      return 0;
    }
    if (uint64_t(offset) > std::numeric_limits<size_t>::max()) {
      return -EOVERFLOW;
    }
    size_t start = offset;
    if (start >= node->data.size()) {
      return 0;
    }
    size_t count = std::min(length, node->data.size() - start);
    std::copy_n(node->data.data() + start, count, buffer);
    return count;
  }

  ssize_t writeFile(const std::shared_ptr<ProfileNamespaceNode>& node,
                    const uint8_t* buffer,
                    size_t length,
                    off_t offset) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!node || node->kind != ProfileNamespaceNodeKind::File || offset < 0) {
      return -EINVAL;
    }
    if (length == 0) {
      return 0;
    }
    uint64_t end;
    if (__builtin_add_overflow(uint64_t(offset), uint64_t(length), &end) ||
        end > kMaxProfileNamespacePayloadBytes ||
        end > std::numeric_limits<size_t>::max()) {
      return -EFBIG;
    }
    node->data.resize(end);
    std::copy_n(buffer, length, node->data.data() + offset);
    if (int error = commitLocked()) {
      return error;
    }
    return length;
  }

  int resizeFile(const std::shared_ptr<ProfileNamespaceNode>& node,
                 off_t size) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!node || node->kind != ProfileNamespaceNodeKind::File || size < 0) {
      return -EINVAL;
    }
    if (uint64_t(size) > kMaxProfileNamespacePayloadBytes) {
      return -EFBIG;
    }
    node->data.resize(size);
    return commitLocked();
  }

  int flushNamespace() {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (int error = flushContainerLocked()) {
      return poisonLocked(error);
    }
    return 0;
  }

  std::shared_ptr<File> getChild(
    const std::shared_ptr<ProfileNamespaceNode>& directory,
    const std::string& name,
    int* error) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      *error = operation.getError();
      return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      *error = fatalError;
      return nullptr;
    }
    if (!directory || directory->kind != ProfileNamespaceNodeKind::Directory ||
        !validEntryName(name)) {
      *error = -EIO;
      return nullptr;
    }
    auto child = directory->children.find(name);
    if (child == directory->children.end()) {
      *error = 0;
      return nullptr;
    }
    *error = 0;
    return makeFile(child->second);
  }

  std::shared_ptr<DataFile> insertDataFile(
    const std::shared_ptr<ProfileNamespaceNode>& directory,
    const std::string& name,
    mode_t mode) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError || !directory ||
        directory->kind != ProfileNamespaceNodeKind::Directory ||
        !validEntryName(name) || directory->children.count(name)) {
      return nullptr;
    }
    if (nextNodeID == std::numeric_limits<uint64_t>::max()) {
      return nullptr;
    }
    auto node = std::make_shared<ProfileNamespaceNode>(
      ProfileNamespaceNodeKind::File, nextNodeID++, mode & (S_IRWXUGO | S_ISVTX));
    node->parent = directory;
    directory->children.emplace(name, node);
    if (commitLocked()) {
      return nullptr;
    }
    return makeDataFile(node);
  }

  std::shared_ptr<Directory> insertDirectory(
    const std::shared_ptr<ProfileNamespaceNode>& directory,
    const std::string& name,
    mode_t mode) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError || !directory ||
        directory->kind != ProfileNamespaceNodeKind::Directory ||
        !validEntryName(name) || directory->children.count(name)) {
      return nullptr;
    }
    if (nextNodeID == std::numeric_limits<uint64_t>::max()) {
      return nullptr;
    }
    auto node = std::make_shared<ProfileNamespaceNode>(
      ProfileNamespaceNodeKind::Directory,
      nextNodeID++,
      mode & (S_IRWXUGO | S_ISVTX));
    node->parent = directory;
    directory->children.emplace(name, node);
    if (commitLocked()) {
      return nullptr;
    }
    return makeDirectory(node);
  }

  int removeChild(const std::shared_ptr<ProfileNamespaceNode>& directory,
                  const std::string& name) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!directory || directory->kind != ProfileNamespaceNodeKind::Directory ||
        !validEntryName(name)) {
      return -EIO;
    }
    auto child = directory->children.find(name);
    if (child == directory->children.end()) {
      return -ENOENT;
    }
    child->second->parent.reset();
    directory->children.erase(child);
    return commitLocked();
  }

  int moveChild(const std::shared_ptr<ProfileNamespaceNode>& directory,
                const std::string& name,
                const std::shared_ptr<File>& file);

  ssize_t getNumEntries(const std::shared_ptr<ProfileNamespaceNode>& directory) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return fatalError;
    }
    if (!directory || directory->kind != ProfileNamespaceNodeKind::Directory) {
      return -EIO;
    }
    return directory->children.size();
  }

  Directory::MaybeEntries getEntries(
    const std::shared_ptr<ProfileNamespaceNode>& directory) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return {operation.getError()};
    }
    std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
    if (fatalError) {
      return {fatalError};
    }
    if (!directory || directory->kind != ProfileNamespaceNodeKind::Directory) {
      return {-EIO};
    }
    std::vector<Directory::Entry> result;
    result.reserve(directory->children.size());
    for (const auto& [name, child] : directory->children) {
      result.push_back({name,
                        child->kind == ProfileNamespaceNodeKind::File
                          ? File::DataFileKind
                          : File::DirectoryKind,
                        0});
    }
    return {std::move(result)};
  }

  std::shared_ptr<DataFile> createFile(mode_t) override { return nullptr; }
  std::shared_ptr<Directory> createDirectory(mode_t mode) override;
  std::shared_ptr<Symlink> createSymlink(std::string) override {
    return nullptr;
  }

  int prepareOPFSProfileRetirement(bool checkResources) override {
    int firstError = 0;
    {
      ProfileLeaseState::InternalOperation operation(*profileLeaseState);
      if (!operation) {
        firstError = operation.getError();
      } else {
        std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
        // Both physical files retain a SyncAccessHandle while the backend is
        // live. Close them before the inherited preflight observes browser
        // resources; the journal remains permanent after a valid PREPARED or
        // PUBLISHED record and is never removed during ordinary drain.
        firstError = closeContainerForRetirementLocked(!fatalError);
        if (!firstError) {
          firstError = closeJournalForRetirementLocked(!fatalError);
        }
        if (!firstError && discardUnpublishedJournal && !fatalError) {
          // This is the known factory-failure gap after creating a journal
          // name but before PREPARED was durably acknowledged. No canonical
          // file was permitted on that path, so confirmed removal is safe.
          firstError = removePhysicalChildLocked(journalName);
          if (!firstError) {
            discardUnpublishedJournal = false;
          }
        }
        if (!firstError && fatalError) {
          firstError = fatalError;
        }
      }
    }
    int inherited = OPFSBackend::prepareOPFSProfileRetirement(
      checkResources && firstError == 0);
    return firstError ? firstError : inherited;
  }
};

class ProfileNamespaceDataFile : public DataFile {
  ProfileNamespaceBackend* backend;
  std::shared_ptr<ProfileNamespaceNode> node;

  off_t getSize() override { return backend->getFileSize(node); }
  int open(oflags_t flags) override { return backend->openFile(node, flags); }
  int close() override { return backend->closeFile(node); }
  ssize_t read(uint8_t* buffer, size_t length, off_t offset) override {
    return backend->readFile(node, buffer, length, offset);
  }
  ssize_t write(const uint8_t* buffer, size_t length, off_t offset) override {
    return backend->writeFile(node, buffer, length, offset);
  }
  int setSize(off_t size) override { return backend->resizeFile(node, size); }
  int flush() override { return backend->flushNamespace(); }

public:
  ProfileNamespaceDataFile(ProfileNamespaceBackend* backend,
                           std::shared_ptr<ProfileNamespaceNode> node)
    : DataFile(node->mode, backend), backend(backend), node(std::move(node)) {}

  const std::shared_ptr<ProfileNamespaceNode>& getNode() const { return node; }
};

class ProfileNamespaceDirectory : public Directory {
  ProfileNamespaceBackend* backend;
  std::shared_ptr<ProfileNamespaceNode> node;

  std::shared_ptr<File> getChild(const std::string& name) override {
    int error = 0;
    auto child = backend->getChild(node, name, &error);
    return error ? nullptr : child;
  }

  MaybeFile getChildWithError(const std::string& name) override {
    int error = 0;
    auto child = backend->getChild(node, name, &error);
    return error ? MaybeFile(error) : MaybeFile(std::move(child));
  }

  std::shared_ptr<DataFile> insertDataFile(const std::string& name,
                                           mode_t mode) override {
    return backend->insertDataFile(node, name, mode);
  }

  std::shared_ptr<Directory> insertDirectory(const std::string& name,
                                             mode_t mode) override {
    return backend->insertDirectory(node, name, mode);
  }

  std::shared_ptr<Symlink> insertSymlink(const std::string&,
                                         const std::string&) override {
    return nullptr;
  }

  int insertMove(const std::string& name, std::shared_ptr<File> file) override {
    return backend->moveChild(node, name, file);
  }

  int removeChild(const std::string& name) override {
    return backend->removeChild(node, name);
  }

  ssize_t getNumEntries() override { return backend->getNumEntries(node); }

  MaybeEntries getEntries() override { return backend->getEntries(node); }

  int flush() override { return backend->flushNamespace(); }

public:
  ProfileNamespaceDirectory(ProfileNamespaceBackend* backend,
                            std::shared_ptr<ProfileNamespaceNode> node)
    : Directory(node->mode, backend), backend(backend), node(std::move(node)) {}

  const std::shared_ptr<ProfileNamespaceNode>& getNode() const { return node; }
};

std::shared_ptr<File> ProfileNamespaceBackend::makeFile(
  const std::shared_ptr<ProfileNamespaceNode>& node) {
  if (!node) {
    return nullptr;
  }
  if (node->kind == ProfileNamespaceNodeKind::File) {
    return makeDataFile(node);
  }
  if (node->kind == ProfileNamespaceNodeKind::Directory) {
    return makeDirectory(node);
  }
  return nullptr;
}

std::shared_ptr<DataFile> ProfileNamespaceBackend::makeDataFile(
  const std::shared_ptr<ProfileNamespaceNode>& node) {
  if (!node || node->kind != ProfileNamespaceNodeKind::File) {
    return nullptr;
  }
  return std::make_shared<ProfileNamespaceDataFile>(this, node);
}

std::shared_ptr<Directory> ProfileNamespaceBackend::makeDirectory(
  const std::shared_ptr<ProfileNamespaceNode>& node) {
  if (!node || node->kind != ProfileNamespaceNodeKind::Directory) {
    return nullptr;
  }
  return std::make_shared<ProfileNamespaceDirectory>(this, node);
}

int ProfileNamespaceBackend::moveChild(
  const std::shared_ptr<ProfileNamespaceNode>& directory,
  const std::string& name,
  const std::shared_ptr<File>& file) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return operation.getError();
  }
  std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
  if (fatalError) {
    return fatalError;
  }
  if (!directory || directory->kind != ProfileNamespaceNodeKind::Directory ||
      !validEntryName(name) || !file || file->getBackend() != this) {
    return -EXDEV;
  }
  std::shared_ptr<ProfileNamespaceNode> source;
  if (file->is<ProfileNamespaceDataFile>()) {
    source = std::static_pointer_cast<ProfileNamespaceDataFile>(file)->getNode();
  } else if (file->is<ProfileNamespaceDirectory>()) {
    source = std::static_pointer_cast<ProfileNamespaceDirectory>(file)->getNode();
  } else {
    return -EXDEV;
  }
  auto oldParent = source->parent.lock();
  if (!oldParent || oldParent->kind != ProfileNamespaceNodeKind::Directory) {
    return -ENOENT;
  }
  for (auto ancestor = directory; ancestor; ancestor = ancestor->parent.lock()) {
    if (ancestor == source) {
      return -EINVAL;
    }
  }
  auto oldEntry = std::find_if(
    oldParent->children.begin(), oldParent->children.end(),
    [&](const auto& entry) { return entry.second == source; });
  if (oldEntry == oldParent->children.end()) {
    return -EIO;
  }
  if (auto existing = directory->children.find(name);
      existing != directory->children.end()) {
    if (existing->second == source) {
      return 0;
    }
    existing->second->parent.reset();
    directory->children.erase(existing);
  }
  oldParent->children.erase(oldEntry);
  source->parent = directory;
  directory->children.emplace(name, source);
  return commitLocked();
}

std::shared_ptr<Directory> ProfileNamespaceBackend::createDirectory(mode_t mode) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(namespaceMutex);
  if (fatalError || rootExposed) {
    return nullptr;
  }
  const mode_t requestedMode = mode & (S_IRWXUGO | S_ISVTX);
  if (bootstrapState == ProfileNamespaceBootstrapState::Prepared) {
    if (!root) {
      // PREPARED was flushed before this first canonical create. A failure at
      // any point before the PUBLISHED record is recoverable only by a fresh
      // factory under that durable authority; do not expose the root here.
      if (int error = insertPhysicalFileLocked(containerName, false)) {
        poisonLocked(error);
        return nullptr;
      }
      makeContainer();
      if (int error = openContainerLocked()) {
        poisonLocked(error);
        return nullptr;
      }
      root = std::make_shared<ProfileNamespaceNode>(
        ProfileNamespaceNodeKind::Directory, nextNodeID++, requestedMode);
      generation = 0;
      appendOffset = kProfileNamespacePayloadOffset;
      if (commitLocked()) {
        return nullptr;
      }
    } else {
      // A valid PREPARED container can exist only from an interrupted first
      // mount. It has never been exposed, so make the current caller's mode
      // the durable initial mode before publishing the journal decision.
      if (root->kind != ProfileNamespaceNodeKind::Directory ||
          !root->children.empty()) {
        poisonLocked(-EIO);
        return nullptr;
      }
      root->mode = requestedMode;
      if (commitLocked()) {
        return nullptr;
      }
    }
    if (int error = writeJournalLocked(
          ProfileNamespaceBootstrapState::Published)) {
      poisonLocked(error);
      return nullptr;
    }
  }
  if (bootstrapState == ProfileNamespaceBootstrapState::Published &&
      !journalPublicationMirrored) {
    // A fresh factory recovered PREPARED(g1)+PUBLISHED(g2) after the first
    // durable publication record but before its mirror. The root has never
    // been exposed, so it must still be the initial empty root; retain the
    // first record's mode and make the second PUBLISHED record durable before
    // returning it to this caller.
    if (!root || root->kind != ProfileNamespaceNodeKind::Directory ||
        !root->children.empty()) {
      poisonLocked(-EIO);
      return nullptr;
    }
    if (int error = finishPublishedJournalLocked()) {
      poisonLocked(error);
      return nullptr;
    }
  }
  if (bootstrapState != ProfileNamespaceBootstrapState::Published || !root) {
    return nullptr;
  }
  rootExposed = true;
  return makeDirectory(root);
}

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
  bool acquireProxyCompleted = false;
  int err = backend->acquireProfileLease(profile_name, &acquireProxyCompleted);
  if (err != 0) {
    assert(err < 0);
    if (acquireProxyCompleted) {
      wasmFS.cancelTerminalLeaseOwnerReservation();
    } else {
      wasmFS.markTerminalLeaseOwnerReservationAmbiguous();
      backend->abandonUnacknowledgedProfileLeaseAcquire();
    }
    errno = -err;
    return nullptr;
  }
  return wasmFS.addBackend(std::move(backend));
#endif
}

backend_t wasmfs_create_opfs_profile_namespace_backend(
  const char* profile_name) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    errno = operation.getError();
    return NullBackend;
  }
  if (!IsValidProfileLeaseName(profile_name)) {
    errno = EINVAL;
    return NullBackend;
  }

#ifndef __EMSCRIPTEN_PTHREADS__
  // The namespace selector and the cooperative lease must be owned by one
  // dedicated OPFS worker.  Asyncify/JSPI call sites do not provide that
  // isolated lifetime.
  errno = ENOTSUP;
  return NullBackend;
#else
  if (!wasmFS.reserveTerminalLeaseOwner()) {
    errno = EBUSY;
    return NullBackend;
  }
  assert(!emscripten_is_main_browser_thread() &&
         "Cannot safely create leased OPFS backend on main browser thread");

  auto backend = std::make_unique<ProfileNamespaceBackend>();
  bool acquireProxyCompleted = false;
  int error = backend->acquireProfileLease(
    profile_name, &acquireProxyCompleted);
  if (error) {
    assert(error < 0);
    if (acquireProxyCompleted) {
      // No lease was acknowledged, so mirror the direct leased-backend
      // factory and release the local reservation. Do not run the leased
      // drain protocol while ProfileLeaseState is still Unleased.
      wasmFS.cancelTerminalLeaseOwnerReservation();
    } else {
      // The browser may own a Web Lock even though native proxying never
      // observed completion. Retain this worker/reservation tombstone rather
      // than allowing another backend in the same WasmFS instance to retry.
      wasmFS.markTerminalLeaseOwnerReservationAmbiguous();
      backend->abandonUnacknowledgedProfileLeaseAcquire();
    }
    errno = -error;
    return NullBackend;
  }
  error = backend->initialise(profile_name);
  if (error) {
    assert(error < 0);
    // `abandonFailedInitialisation` permits cancellation only after the
    // complete release-and-retirement handoff. An ambiguous or incomplete
    // outcome deliberately leaves a terminally visible reservation and a
    // detached worker tombstone in this WasmFS instance.
    if (backend->abandonFailedInitialisation()) {
      wasmFS.cancelTerminalLeaseOwnerReservation();
    } else {
      wasmFS.markTerminalLeaseOwnerReservationAmbiguous();
    }
    errno = -error;
    return NullBackend;
  }
  return wasmFS.addBackend(std::move(backend));
#endif
}

void EMSCRIPTEN_KEEPALIVE _wasmfs_opfs_record_entry(
  std::vector<Directory::Entry>* entries, const char* name, int type) {
  entries->push_back({name, File::FileKind(type), 0});
}

} // extern "C"
