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

#ifndef WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE
#define WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE 0
#endif

#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE < 0 || \
  WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE > 5
#error "invalid direct OPFS directory proxy completion failure selector"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE
#define WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE 0
#endif

#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE > 1
#error "invalid profile-log V2 proxy completion failure selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE
// This is a narrow test trace: once V2 synthetically enters the terminal
// post-acknowledgement-loss state, later Worker::proxy calls are counted. The
// regression asserts none occur through its explicit failed drain; it does
// not observe runtime destruction after the iframe is disposed. The normal
// runtime has neither the flag nor the counter.
std::atomic<bool> profileLogV2ProxyCompletionLatchForTesting = false;
std::atomic<uint32_t> profileLogV2ProxiesAfterLatchForTesting = 0;

void latchProfileLogV2ProxyCompletionForTesting() {
  profileLogV2ProxyCompletionLatchForTesting.store(true);
}
#endif

#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 2
constexpr char kOPFSDirectOperationAdmissionRaceLatcherTestName[] =
  "__wasmfs_opfs_test_direct_operation_admission_latcher__";

enum DirectOperationAdmissionRaceState {
  DirectOperationAdmissionRaceIdle,
  DirectOperationAdmissionRaceArmed,
  DirectOperationAdmissionRaceLatcherPaused,
  DirectOperationAdmissionRaceFollowerBlocked,
  DirectOperationAdmissionRaceFollowerBypassed,
  DirectOperationAdmissionRaceContinue,
};

// This test-only barrier proves that an in-flight direct operation owns the
// ambiguity admission domain until it either gets a completion or latches an
// unacknowledged one. It is compiled only into the existing phase-2 fault
// variation, never into normal direct or namespace builds.
std::atomic<int> directOperationAdmissionRaceState =
  DirectOperationAdmissionRaceIdle;

void armDirectOperationAdmissionRaceForTesting() {
  int expected = DirectOperationAdmissionRaceIdle;
  assert(directOperationAdmissionRaceState.compare_exchange_strong(
    expected, DirectOperationAdmissionRaceArmed));
}

void pauseDirectOperationAdmissionRaceForTesting() {
  int expected = DirectOperationAdmissionRaceArmed;
  assert(directOperationAdmissionRaceState.compare_exchange_strong(
    expected, DirectOperationAdmissionRaceLatcherPaused));
  while (directOperationAdmissionRaceState.load() !=
         DirectOperationAdmissionRaceContinue) {
    emscripten_thread_sleep(1);
  }
}

void noteDirectOperationAdmissionAttemptForTesting(
  std::recursive_mutex& directOperationMutex) {
  if (directOperationAdmissionRaceState.load() !=
      DirectOperationAdmissionRaceLatcherPaused) {
    return;
  }

  int expected = DirectOperationAdmissionRaceLatcherPaused;
  if (directOperationMutex.try_lock()) {
    directOperationMutex.unlock();
    (void)directOperationAdmissionRaceState.compare_exchange_strong(
      expected, DirectOperationAdmissionRaceFollowerBypassed);
  } else {
    (void)directOperationAdmissionRaceState.compare_exchange_strong(
      expected, DirectOperationAdmissionRaceFollowerBlocked);
  }
}

void continueDirectOperationAdmissionRaceForTesting() {
  int state = directOperationAdmissionRaceState.load();
  assert(state == DirectOperationAdmissionRaceLatcherPaused ||
         state == DirectOperationAdmissionRaceFollowerBlocked ||
         state == DirectOperationAdmissionRaceFollowerBypassed);
  directOperationAdmissionRaceState.store(DirectOperationAdmissionRaceContinue);
}

void resetDirectOperationAdmissionRaceForTesting() {
  int expected = DirectOperationAdmissionRaceContinue;
  assert(directOperationAdmissionRaceState.compare_exchange_strong(
    expected, DirectOperationAdmissionRaceIdle));
}
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

  template<typename T> bool operator()(T func) {
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE
    if (profileLogV2ProxyCompletionLatchForTesting.load()) {
      ++profileLogV2ProxiesAfterLatchForTesting;
    }
#endif
    return proxy(func);
  }

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
// rejected SyncAccessHandle close. A lost ProxyWorker completion is equally
// ambiguous: the browser-side operation may have changed a directory, opened
// or released a handle, even though C++ did not observe its result. Keep this
// per-backend state separately so later wrappers fail closed and a terminal
// drain cannot mistake an already-removed descriptor table for proof that the
// browser-side OPFS state is known.
class TerminalCloseState {
  std::atomic<int> firstError = 0;
  // A completed browser operation can report its own error and still retain
  // the existing terminal-drain semantics. Only a missing native completion
  // acknowledgement makes *other* direct wrappers unable to reason about the
  // browser-side namespace, so keep that narrower global gate separately.
  std::atomic<int> unacknowledgedProxyError = 0;
  // Direct wrappers must check the ambiguity latch and submit their browser
  // callback as one admission step. Otherwise a second wrapper could sample a
  // healthy latch, wait while another wrapper loses its completion, and then
  // issue a browser request after the backend has become ambiguous.
  std::recursive_mutex directOperationMutex;

public:
  void recordFailedAccessClose(int error) {
    if (error >= 0) {
      error = -EIO;
    }
    int expected = 0;
    (void)firstError.compare_exchange_strong(expected, error);
  }

  void recordUnacknowledgedProxyCompletion() {
    int expected = 0;
    (void)unacknowledgedProxyError.compare_exchange_strong(expected, -EIO);
    recordFailedAccessClose(-EIO);
  }

  int getFailedAccessCloseError() const { return firstError.load(); }

  int getUnacknowledgedProxyError() const {
    return unacknowledgedProxyError.load();
  }

  std::recursive_mutex& getDirectOperationMutex() {
    return directOperationMutex;
  }
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

// Direct OPFS wrappers share one terminal error state.  Namespace-backend
// physical helper wrappers pass no failure gate here: they retain their
// established, separately journaled failure protocol rather than inheriting
// the direct backend's global operation fence.
class DirectOPFSOperation {
  ProfileLeaseState::InternalOperation profileOperation;
  std::shared_ptr<TerminalCloseState> terminalCloseState;
  std::unique_lock<std::recursive_mutex> directOperationLock;
  int error = 0;

public:
  DirectOPFSOperation(
    ProfileLeaseState& profileLeaseState,
    std::shared_ptr<TerminalCloseState> terminalCloseState)
    : profileOperation(profileLeaseState),
      terminalCloseState(std::move(terminalCloseState)) {
    if (!profileOperation) {
      error = profileOperation.getError();
    } else if (this->terminalCloseState) {
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 2
      noteDirectOperationAdmissionAttemptForTesting(
        this->terminalCloseState->getDirectOperationMutex());
#endif
      directOperationLock = std::unique_lock(
        this->terminalCloseState->getDirectOperationMutex());
      if ((error =
             this->terminalCloseState->getUnacknowledgedProxyError())) {
        // A completed browser-operation failure preserves its existing local
        // error behavior. Only a lost proxy completion is backend-wide
        // ambiguous and therefore closes this direct-operation gate.
      }
    }
  }

  explicit operator bool() const { return error == 0; }
  int getError() const { return error; }
};

// Cached direct OPFS wrappers can proxy a best-effort JS-handle free from a
// destructor, after ordinary syscall admission has ended. Keep those proxies
// in the same ambiguity admission domain without changing the namespace
// backend's separately journaled physical-helper lifecycle.
class DirectOPFSDestructorOperation {
  ProfileLeaseState::DestructorProxyOperation profileOperation;
  std::unique_lock<std::recursive_mutex> directOperationLock;
  bool admitted = false;

public:
  DirectOPFSDestructorOperation(
    ProfileLeaseState& profileLeaseState,
    std::shared_ptr<TerminalCloseState> terminalCloseState)
    : profileOperation(profileLeaseState) {
    if (!profileOperation) {
      return;
    }
    if (terminalCloseState) {
      directOperationLock = std::unique_lock(
        terminalCloseState->getDirectOperationMutex());
      if (terminalCloseState->getUnacknowledgedProxyError()) {
        return;
      }
    }
    admitted = true;
  }

  explicit operator bool() const { return admitted; }
};

class OpenState {
public:
  enum Kind { None, Access, Blob, FailedAccessClose };

private:
  Kind kind = None;
  int id = -1;
  size_t openCount = 0;
  // A browser operation can report a completed error without making other
  // wrappers' view of OPFS indeterminate. Keep the narrower missing-proxy
  // acknowledgement signal separate from the resource poison kind.
  bool unacknowledgedProxyCompletion = false;

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
              unacknowledgedProxyCompletion = true;
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
              unacknowledgedProxyCompletion = true;
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
        unacknowledgedProxyCompletion = true;
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
        unacknowledgedProxyCompletion = true;
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
            unacknowledgedProxyCompletion = true;
            err = -EIO;
          }
          break;
        case Blob:
          if (!proxy([&]() { _wasmfs_opfs_close_blob(id); })) {
            unacknowledgedProxyCompletion = true;
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

  bool hasUnacknowledgedProxyCompletion() const {
    return unacknowledgedProxyCompletion;
  }

  // A cancelled proxy operation does not tell us whether the browser-side
  // operation ran.  Do not let a later operation (especially a close) make a
  // second request against that ambiguous handle state.
  void poison() {
    unacknowledgedProxyCompletion = true;
    kind = FailedAccessClose;
  }
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
  // Direct OPFS passes the backend-wide terminal state here. The namespace
  // backend leaves it null so its physical helper wrappers keep their own
  // journaled recovery behavior.
  std::shared_ptr<TerminalCloseState> directOperationFailureState;

  int recordProxyFailure() {
    // This covers cancelled access/blob operations as well as a cancelled
    // idle FileSystemFileHandle operation.  A later close cannot safely
    // distinguish an operation that never reached the worker from one whose
    // completion acknowledgement was lost.
    state.poison();
    terminalCloseState->recordUnacknowledgedProxyCompletion();
    return -EIO;
  }

  int rejectDirectOperation(const DirectOPFSOperation& operation) {
    // A later wrapper can encounter the backend-wide lost-completion latch
    // before it has made any browser request itself. Convert its locally open
    // state into a no-proxy tombstone so detached descriptors and eventual
    // wrapper destruction cannot try to close or free a browser handle after
    // the OPFS state has become ambiguous.
    if (directOperationFailureState &&
        directOperationFailureState->getUnacknowledgedProxyError()) {
      state.poison();
      failedFileHandleRelease = true;
    }
    return operation.getError();
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
           std::shared_ptr<ProfileLeaseState> profileLeaseState,
           std::shared_ptr<TerminalCloseState> directOperationFailureState)
    : DataFile(mode, backend), proxy(proxy), parentID(parentID),
      name(std::move(name)), terminalCloseState(std::move(terminalCloseState)),
      profileLeaseState(std::move(profileLeaseState)),
      directOperationFailureState(std::move(directOperationFailureState)) {}

  // V2's private physical files are not in WasmFS's descriptor table. When a
  // lost completion prevents any further OPFS proxying, the control backend
  // marks every one of them this way before it abandons its worker. Keep the
  // browser handle ID pinned for document teardown rather than falsely freeing
  // or closing it later.
  void abandonForTerminalFailure() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    state.poison();
    failedFileHandleRelease = true;
  }

  ~OPFSFile() override {
    // A rejected AccessHandle close remains intentionally quarantined in the
    // ProxyWorker until its context is torn down. The file wrapper itself can
    // still be destroyed without pretending the close succeeded.
    assert(state.getKind() == OpenState::None ||
           state.getKind() == OpenState::FailedAccessClose);
    DirectOPFSDestructorOperation destructorOperation(
      *profileLeaseState, directOperationFailureState);
    // Once a proxy completion is lost, even a best-effort free would be a
    // second ambiguous operation.  The scoped worker tombstone owns that
    // browser resource until document teardown instead.
    if (destructorOperation && fileID >= 0 &&
        state.getKind() == OpenState::None && !failedFileHandleRelease) {
      if (!proxy([&]() { _wasmfs_opfs_free_file(fileID); })) {
        terminalCloseState->recordUnacknowledgedProxyCompletion();
      }
    }
  }

  int moveTo(int newParentID, const std::string& newName) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
    if (err && state.hasUnacknowledgedProxyCompletion()) {
      // A completed browser-side open rejection has no live handle to carry
      // into retirement.  Only a lost proxy acknowledgement (or a failed
      // follow-up file-handle release) needs to poison the backend-wide
      // handoff state.
      terminalCloseState->recordUnacknowledgedProxyCompletion();
    }
    return err ? err : releaseError;
  }

  int close() override {
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
    }
    int err = state.close(proxy);
    if (err) {
      // __wasi_fd_close has already removed this descriptor's OpenFileState
      // before it calls us. Remember an ambiguous SyncAccessHandle close at
      // backend scope so terminalDrainFinished() retains a profile lease even
      // when the descriptor table is otherwise empty.
      if (state.hasUnacknowledgedProxyCompletion()) {
        terminalCloseState->recordUnacknowledgedProxyCompletion();
      } else {
        terminalCloseState->recordFailedAccessClose(err);
      }
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return rejectDirectOperation(operation);
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
  std::shared_ptr<TerminalCloseState> directOperationFailureState;

  OPFSDirectory(mode_t mode,
                backend_t backend,
                int dirID,
                Worker& proxy,
                std::shared_ptr<TerminalCloseState> terminalCloseState,
                std::shared_ptr<ProfileLeaseState> profileLeaseState,
                std::shared_ptr<TerminalCloseState> directOperationFailureState)
    : Directory(mode, backend), proxy(proxy), dirID(dirID),
      terminalCloseState(std::move(terminalCloseState)),
      profileLeaseState(std::move(profileLeaseState)),
      directOperationFailureState(std::move(directOperationFailureState)) {}

  ~OPFSDirectory() override {
    // The root handle is shared by all mounts of this backend, so only child
    // directory handles are owned by their C++ wrappers. Slot 0 is reserved.
    DirectOPFSDestructorOperation destructorOperation(
      *profileLeaseState, directOperationFailureState);
    if (destructorOperation && dirID != 0 && dirID != kOPFSRootDirectoryID &&
        !proxy([&]() { _wasmfs_opfs_free_directory(dirID); })) {
      terminalCloseState->recordUnacknowledgedProxyCompletion();
    }
  }

private:
  int recordProxyFailure() {
    terminalCloseState->recordUnacknowledgedProxyCompletion();
    return -EIO;
  }

  off_t getSize() override {
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
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
      return recordProxyFailure();
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
        profileLeaseState,
        directOperationFailureState);
    }
    if (childType == 2 && childID > kOPFSRootDirectoryID) {
      return std::make_shared<OPFSDirectory>(
        0777,
        getBackend(),
        childID,
        proxy,
        terminalCloseState,
        profileLeaseState,
        directOperationFailureState);
    }
    // Neither a malformed JS result nor a cancelled/failed proxy is a missing
    // child. Propagate EIO to the syscall layer instead of trapping or
    // fabricating ENOENT.
    return -EIO;
  }

  std::shared_ptr<DataFile> insertDataFile(const std::string& name,
                                           mode_t mode) override {
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return nullptr;
    }
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 2
    if (name == kOPFSDirectOperationAdmissionRaceLatcherTestName) {
      // The DirectOPFSOperation above already owns the shared admission
      // mutex. Pause here so a second directory can prove it blocks at that
      // mutex rather than issuing a browser request before this operation
      // latches its deliberately lost completion.
      pauseDirectOperationAdmissionRaceForTesting();
    }
#endif
    int childID = -EIO;
    bool proxyCompleted = proxy([&](auto ctx) {
      _wasmfs_opfs_insert_file(ctx.ctx, dirID, name.c_str(), &childID);
    });
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 2
    // Model a callback that did reach the OPFS worker but whose native
    // completion acknowledgement was lost.
    if (proxyCompleted) {
      proxyCompleted = false;
    }
#endif
    if (!proxyCompleted || childID != 0) {
      if (!proxyCompleted) {
        recordProxyFailure();
      }
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
      profileLeaseState,
      directOperationFailureState);
  }

  std::shared_ptr<Directory> insertDirectory(const std::string& name,
                                             mode_t mode) override {
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return nullptr;
    }
    int childID = -EIO;
    bool proxyCompleted = proxy([&](auto ctx) {
      _wasmfs_opfs_insert_directory(ctx.ctx, dirID, name.c_str(), &childID);
    });
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 3
    // Model a callback that did reach the OPFS worker but whose native
    // completion acknowledgement was lost.
    if (proxyCompleted) {
      proxyCompleted = false;
    }
#endif
    if (!proxyCompleted || childID <= kOPFSRootDirectoryID) {
      if (!proxyCompleted) {
        recordProxyFailure();
      }
      // TODO: Propagate specific errors.
      return nullptr;
    }
    return std::make_shared<OPFSDirectory>(
      mode,
      getBackend(),
      childID,
      proxy,
      terminalCloseState,
      profileLeaseState,
      directOperationFailureState);
  }

  std::shared_ptr<Symlink> insertSymlink(const std::string& name,
                                         const std::string& target) override {
    // Symlinks not supported.
    // TODO: Propagate EPERM specifically.
    return nullptr;
  }

  int insertMove(const std::string& name, std::shared_ptr<File> file) override {
    if (file->is<DataFile>()) {
      // moveTo() owns the direct-operation admission around its browser
      // callback. Do not take it here as well: rename holds this destination
      // directory lock, while moveTo() takes the source-file lock before its
      // own admission, and an unrelated source-file operation can otherwise
      // form a directory-gate/file-lock cycle.
      auto opfsFile = std::static_pointer_cast<OPFSFile>(file);
      return opfsFile->moveTo(dirID, name);
    }

    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return operation.getError();
    }
    // TODO: Support moving directories once OPFS supports that.
    // EBUSY can be returned when the directory is "in use by the system,"
    // which can mean whatever we want.
    return -EBUSY;
  }

  int removeChild(const std::string& name) override {
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return operation.getError();
    }
    int err = 0;
    bool proxyCompleted = proxy([&](auto ctx) {
      _wasmfs_opfs_remove_child(ctx.ctx, dirID, name.c_str(), &err);
    });
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 4
    // Model a callback that did reach the OPFS worker but whose native
    // completion acknowledgement was lost.
    if (proxyCompleted) {
      proxyCompleted = false;
    }
#endif
    if (!proxyCompleted) {
      return recordProxyFailure();
    }
    return err;
  }

  ssize_t getNumEntries() override {
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
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
    DirectOPFSOperation operation(
      *profileLeaseState, directOperationFailureState);
    if (!operation) {
      return {operation.getError()};
    }
    std::vector<Directory::Entry> entries;
    int err = 0;
    bool proxyCompleted = proxy([&](auto ctx) {
      _wasmfs_opfs_get_entries(ctx.ctx, dirID, &entries, &err);
    });
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 5
    // Model a callback that did reach the OPFS worker but whose native
    // completion acknowledgement was lost.
    if (proxyCompleted) {
      proxyCompleted = false;
    }
#endif
    if (!proxyCompleted) {
      return {recordProxyFailure()};
    }
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
    DirectOPFSOperation operation(*profileLeaseState, terminalCloseState);
    if (!operation) {
      return nullptr;
    }
    bool proxyCompleted = proxy(
      [](auto ctx) { _wasmfs_opfs_init_root_directory(ctx.ctx); });
#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 1
    // Model a callback that did reach the OPFS worker but whose native
    // completion acknowledgement was lost.
    if (proxyCompleted) {
      proxyCompleted = false;
    }
#endif
    if (!proxyCompleted) {
      terminalCloseState->recordUnacknowledgedProxyCompletion();
      return nullptr;
    }
    return std::make_shared<OPFSDirectory>(
      mode,
      this,
      kOPFSRootDirectoryID,
      proxy,
      terminalCloseState,
      profileLeaseState,
      terminalCloseState);
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
      profileLeaseState,
      nullptr);
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
      profileLeaseState,
      nullptr);
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

// V2 is intentionally a control-plane experiment rather than a replacement
// for ProfileNamespaceBackend above.  In particular, it has no directory
// tree, data-file, metadata, or Chrome integration surface.  Its purpose is
// to make one sharply bounded recovery property executable: a fresh document
// can select either an old or a new opaque root only from fixed regular files,
// without depending on OPFS directory fsync or directory rename.
constexpr size_t kProfileLogV2RecordSize = 64;
constexpr size_t kProfileLogV2BootstrapSize = 2 * kProfileLogV2RecordSize;
constexpr size_t kProfileLogV2ControlSize = 6 * kProfileLogV2RecordSize;
constexpr size_t kProfileLogV2RootSize = kProfileLogV2RecordSize;
constexpr uint64_t kProfileLogV2MaxSafeOffset = UINT64_C(9007199254740991);
static_assert(kProfileLogV2BootstrapSize <= kProfileLogV2MaxSafeOffset);
static_assert(kProfileLogV2ControlSize <= kProfileLogV2MaxSafeOffset);
static_assert(kProfileLogV2RootSize <= kProfileLogV2MaxSafeOffset);
constexpr uint32_t kProfileLogV2FormatVersion = 2;
constexpr uint32_t kProfileLogV2BootstrapReady = 1;
constexpr uint32_t kProfileLogV2PhaseClean = 1;
constexpr std::array<uint8_t, 8> kProfileLogV2BootstrapMagic = {
  'W', 'F', 'S', 'L', 'G', '2', 'B', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV2RootMagic = {
  'W', 'F', 'S', 'L', 'G', '2', 'R', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV2DescriptorMagic = {
  'W', 'F', 'S', 'L', 'G', '2', 'D', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV2PhaseMagic = {
  'W', 'F', 'S', 'L', 'G', '2', 'P', '0'};

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT
#define WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION
#define WASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION 0
#endif

#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT > 1
#error "invalid profile-log V2 interruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION > 2
#error "invalid profile-log V2 selected-control corruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT
// The browser regression defines this only in a link variation that pauses
// after bootstrap witness zero, or after the first or second flushed CLEAN
// phase witness. It never provides a production recovery mechanism and is
// deliberately not an OPFS write hook.
extern "C" void wasmfs_opfs_profile_log_v2_test_maybe_interrupt(
  int checkpoint);
#endif

struct ProfileLogV2RootImage {
  uint64_t generation;
  uint64_t value;
  uint64_t checksum;
};

struct ProfileLogV2Descriptor {
  uint64_t generation;
  uint32_t rootSlot;
  uint64_t rootChecksum;
  uint64_t recordChecksum;
};

struct ProfileLogV2Phase {
  uint64_t generation;
  uint32_t rootSlot;
  uint64_t descriptorChecksum;
};

uint64_t profileLogV2Checksum(const uint8_t* data, size_t size) {
  uint64_t result = UINT64_C(1469598103934665603);
  for (size_t i = 0; i != size; ++i) {
    result ^= data[i];
    result *= UINT64_C(1099511628211);
  }
  return result;
}

template <size_t Size>
bool profileLogV2HasZeroTail(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  return std::all_of(data.begin() + offset,
                     data.end(),
                     [](uint8_t value) { return value == 0; });
}

template <size_t Size>
void writeProfileLogV2U32(std::array<uint8_t, Size>& data,
                          size_t offset,
                          uint32_t value) {
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <size_t Size>
void writeProfileLogV2U64(std::array<uint8_t, Size>& data,
                          size_t offset,
                          uint64_t value) {
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <size_t Size>
uint32_t readProfileLogV2U32(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value |= uint32_t(data[offset + i]) << (8 * i);
  }
  return value;
}

template <size_t Size>
uint64_t readProfileLogV2U64(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value |= uint64_t(data[offset + i]) << (8 * i);
  }
  return value;
}

class ProfileLogV2ControlBackend final : public OPFSBackend {
  std::recursive_mutex controlMutex;
  std::shared_ptr<OPFSDirectory> physicalRoot;
  std::shared_ptr<OPFSFile> bootstrap;
  std::shared_ptr<OPFSFile> control;
  std::array<std::shared_ptr<OPFSFile>, 2> roots;
  std::array<bool, 2> rootsOpen = {};
  bool bootstrapOpen = false;
  bool controlOpen = false;
  bool initialisationAmbiguous = false;
  uint64_t profileChecksum = 0;
  uint32_t profileLength = 0;
  uint64_t generation = 0;
  uint64_t rootValue = 0;
  // A fresh open can safely *read* the old root when exactly one CLEAN
  // witness names the next generation, but it must not overwrite that known
  // partial transaction with another commit. A future V2 extension may add a
  // separately durable repair decision; this small primitive deliberately
  // exposes only the conservative read-and-drain outcome.
  bool recoveryNeedsRepair = false;
  int fatalError = 0;
  std::string bootstrapName;
  std::string controlName;
  std::array<std::string, 2> rootNames;

  static std::string storageStem(std::string_view profileName) {
    // Length-delimiting keeps profile names injective without asking OPFS for
    // any directory operation during a transaction.
    std::string result = ".wasmfs-profile-log-v2-";
    result += std::to_string(profileName.size());
    result += '-';
    result.append(profileName.data(), profileName.size());
    return result;
  }

  static uint64_t descriptorOffset(uint64_t generation, uint64_t copy) {
    // Two independently checksummed descriptor witnesses are retained for
    // each parity. The next generation overwrites only the inactive parity,
    // leaving the old CLEAN root recoverable until the new CLEAN quorum.
    return ((generation & 1) * 2 + copy) * kProfileLogV2RecordSize;
  }

  static uint64_t phaseOffset(uint64_t copy) {
    return (4 + copy) * kProfileLogV2RecordSize;
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

  int recordInitialisationProxyFailure() {
    initialisationAmbiguous = true;
    terminalCloseState->recordUnacknowledgedProxyCompletion();
    return -EIO;
  }

  std::shared_ptr<OPFSFile> makePhysicalFile(const std::string& name) {
    auto file = std::make_shared<OPFSFile>(0600,
                                           this,
                                           kOPFSRootDirectoryID,
                                           name,
                                           proxy,
                                           terminalCloseState,
                                           profileLeaseState,
                                           terminalCloseState);
    file->locked().setParent(physicalRoot);
    return file;
  }

  int initialisePhysicalRootLocked() {
    DirectOPFSOperation operation(*profileLeaseState, terminalCloseState);
    if (!operation) {
      return operation.getError();
    }
    if (!proxy([](auto ctx) { _wasmfs_opfs_init_root_directory(ctx.ctx); })) {
      return recordInitialisationProxyFailure();
    }
    physicalRoot = std::make_shared<OPFSDirectory>(0700,
                                                    this,
                                                    kOPFSRootDirectoryID,
                                                    proxy,
                                                    terminalCloseState,
                                                    profileLeaseState,
                                                    terminalCloseState);
    return 0;
  }

  int lookupPhysicalFileLocked(const std::string& name) {
    DirectOPFSOperation operation(*profileLeaseState, terminalCloseState);
    if (!operation) {
      return operation.getError();
    }
    int childType = 0;
    int childID = -EIO;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_get_child(ctx.ctx,
                                 kOPFSRootDirectoryID,
                                 name.c_str(),
                                 &childType,
                                 &childID);
        })) {
      return recordInitialisationProxyFailure();
    }
    if (childID == -ENOENT) {
      return -ENOENT;
    }
    // OPFS files deliberately retain no JS file-handle ID after creation, so
    // a regular child is represented by type 1 and id 0.
    return childType == 1 && childID == 0 ? 0 : -EIO;
  }

  int insertPhysicalFileLocked(const std::string& name) {
    DirectOPFSOperation operation(*profileLeaseState, terminalCloseState);
    if (!operation) {
      return operation.getError();
    }
    int childID = -EIO;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_insert_file(
            ctx.ctx, kOPFSRootDirectoryID, name.c_str(), &childID);
        })) {
      return recordInitialisationProxyFailure();
    }
    return childID == 0 ? 0 : childID < 0 ? childID : -EIO;
  }

  int openFileLocked(const std::shared_ptr<OPFSFile>& file, bool* opened) {
    if (!file || !opened) {
      return -EIO;
    }
    int error = file->locked().open(O_RDWR);
    if (!error) {
      *opened = true;
    }
    return error;
  }

  int openAllFilesLocked() {
    if (int error = openFileLocked(bootstrap, &bootstrapOpen)) {
      return error;
    }
    if (int error = openFileLocked(control, &controlOpen)) {
      return error;
    }
    for (size_t i = 0; i != roots.size(); ++i) {
      if (int error = openFileLocked(roots[i], &rootsOpen[i])) {
        return error;
      }
    }
    return 0;
  }

  int closeFileForRetirementLocked(const std::shared_ptr<OPFSFile>& file,
                                   bool* opened,
                                   bool flushFirst) {
    if (!opened || !*opened) {
      return 0;
    }
    int firstError = flushFirst ? file->locked().flush() : 0;
    int closeError = file->locked().close();
    if (!closeError) {
      *opened = false;
    } else if (!firstError) {
      firstError = closeError;
    }
    return firstError;
  }

  int closeAllFilesForRetirementLocked(bool flushFirst) {
    // A completed flush/close error is not an ambiguity grant: keep closing
    // the remaining private AccessHandles so a later failed handoff cannot
    // abandon healthy wrappers in Access state. Stop only if a close loses its
    // completion acknowledgement; prepareOPFSProfileRetirement will then turn
    // every wrapper into a no-proxy tombstone before it abandons the worker.
    int firstError = 0;
    auto closeOne = [&](const std::shared_ptr<OPFSFile>& file, bool* opened) {
      int error = closeFileForRetirementLocked(file, opened, flushFirst);
      if (!firstError && error) {
        firstError = error;
      }
      return terminalCloseState->getUnacknowledgedProxyError() == 0;
    };
    if (!closeOne(roots[0], &rootsOpen[0]) ||
        !closeOne(roots[1], &rootsOpen[1]) ||
        !closeOne(control, &controlOpen) ||
        !closeOne(bootstrap, &bootstrapOpen)) {
      return firstError ? firstError
                        : terminalCloseState->getUnacknowledgedProxyError();
    }
    return firstError;
  }

  void abandonPhysicalFilesLocked() {
    // No wrapper reset occurs here: the shared_ptrs remain alive through the
    // destructor-proxy gate, and each retained browser handle belongs to the
    // abandoned worker context until document teardown. This is intentionally
    // a native-only state transition with no OPFS callback.
    if (bootstrap) {
      bootstrap->abandonForTerminalFailure();
    }
    if (control) {
      control->abandonForTerminalFailure();
    }
    for (const auto& root : roots) {
      if (root) {
        root->abandonForTerminalFailure();
      }
    }
  }

  int stopAfterUnacknowledgedProxyLocked() {
    const int error = terminalCloseState->getUnacknowledgedProxyError();
    if (!error) {
      return 0;
    }
    abandonPhysicalFilesLocked();
    // This gate is purely native admission/waiting. Closing it prevents later
    // wrapper destructors from submitting a best-effort handle free before
    // finishOPFSProfileDrain(false) detaches the dedicated worker.
    profileLeaseState->closeDestructorProxying();
    return error;
  }

  int setFixedSizeLocked(const std::shared_ptr<OPFSFile>& file, size_t size) {
    if (!file ||
        size > static_cast<size_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    if (int error = file->locked().setSize(size)) {
      return error;
    }
    return file->locked().flush();
  }

  int requireFixedSizeLocked(const std::shared_ptr<OPFSFile>& file,
                             size_t expectedSize) {
    if (!file || expectedSize >
                   static_cast<size_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    const off_t size = file->locked().getSize();
    if (size < 0) {
      return size;
    }
    return size_t(size) == expectedSize ? 0 : -EIO;
  }

  int writeRecordLocked(const std::shared_ptr<OPFSFile>& file,
                        uint64_t offset,
                        const std::array<uint8_t, kProfileLogV2RecordSize>&
                          record) {
    if (!file || offset > std::numeric_limits<off_t>::max()) {
      return -EOVERFLOW;
    }
    ssize_t written = file->locked().write(record.data(), record.size(), offset);
    if (written < 0) {
      return written;
    }
    if (size_t(written) != record.size()) {
      return -EIO;
    }
    return file->locked().flush();
  }

  int readRecordLocked(const std::shared_ptr<OPFSFile>& file,
                       uint64_t offset,
                       std::array<uint8_t, kProfileLogV2RecordSize>* record) {
    if (!file || !record || offset > std::numeric_limits<off_t>::max()) {
      return -EOVERFLOW;
    }
    ssize_t read = file->locked().read(record->data(), record->size(), offset);
    if (read < 0) {
      return read;
    }
    return size_t(read) == record->size() ? 0 : -EIO;
  }

  std::array<uint8_t, kProfileLogV2RecordSize> makeBootstrapRecord() const {
    std::array<uint8_t, kProfileLogV2RecordSize> record = {};
    std::copy(kProfileLogV2BootstrapMagic.begin(),
              kProfileLogV2BootstrapMagic.end(),
              record.begin());
    writeProfileLogV2U32(record, 8, kProfileLogV2FormatVersion);
    writeProfileLogV2U32(record, 12, kProfileLogV2RecordSize);
    writeProfileLogV2U64(record, 16, profileChecksum);
    writeProfileLogV2U32(record, 24, profileLength);
    writeProfileLogV2U32(record, 28, kProfileLogV2BootstrapReady);
    writeProfileLogV2U64(record, 32, 1);
    writeProfileLogV2U64(record, 40, profileLogV2Checksum(record.data(), 40));
    return record;
  }

  bool parseBootstrapRecord(
    const std::array<uint8_t, kProfileLogV2RecordSize>& record) const {
    return std::equal(kProfileLogV2BootstrapMagic.begin(),
                      kProfileLogV2BootstrapMagic.end(),
                      record.begin()) &&
           readProfileLogV2U32(record, 8) == kProfileLogV2FormatVersion &&
           readProfileLogV2U32(record, 12) == kProfileLogV2RecordSize &&
           readProfileLogV2U64(record, 16) == profileChecksum &&
           readProfileLogV2U32(record, 24) == profileLength &&
           readProfileLogV2U32(record, 28) == kProfileLogV2BootstrapReady &&
           readProfileLogV2U64(record, 32) == 1 &&
           readProfileLogV2U64(record, 40) ==
             profileLogV2Checksum(record.data(), 40) &&
           profileLogV2HasZeroTail(record, 48);
  }

  std::array<uint8_t, kProfileLogV2RecordSize> makeRootRecord(
    uint64_t recordGeneration,
    uint64_t value) const {
    std::array<uint8_t, kProfileLogV2RecordSize> record = {};
    std::copy(kProfileLogV2RootMagic.begin(),
              kProfileLogV2RootMagic.end(),
              record.begin());
    writeProfileLogV2U32(record, 8, kProfileLogV2FormatVersion);
    writeProfileLogV2U32(record, 12, kProfileLogV2RecordSize);
    writeProfileLogV2U64(record, 16, recordGeneration);
    writeProfileLogV2U64(record, 24, value);
    writeProfileLogV2U64(record, 32, profileChecksum);
    writeProfileLogV2U64(record, 40, profileLogV2Checksum(record.data(), 40));
    return record;
  }

  std::optional<ProfileLogV2RootImage> parseRootRecord(
    const std::array<uint8_t, kProfileLogV2RecordSize>& record) const {
    if (!std::equal(kProfileLogV2RootMagic.begin(),
                    kProfileLogV2RootMagic.end(),
                    record.begin()) ||
        readProfileLogV2U32(record, 8) != kProfileLogV2FormatVersion ||
        readProfileLogV2U32(record, 12) != kProfileLogV2RecordSize ||
        readProfileLogV2U64(record, 32) != profileChecksum ||
        readProfileLogV2U64(record, 40) !=
          profileLogV2Checksum(record.data(), 40) ||
        !profileLogV2HasZeroTail(record, 48)) {
      return std::nullopt;
    }
    uint64_t recordGeneration = readProfileLogV2U64(record, 16);
    if (!recordGeneration) {
      return std::nullopt;
    }
    return ProfileLogV2RootImage{recordGeneration,
                                 readProfileLogV2U64(record, 24),
                                 readProfileLogV2U64(record, 40)};
  }

  std::array<uint8_t, kProfileLogV2RecordSize> makeDescriptorRecord(
    uint64_t recordGeneration,
    uint32_t rootSlot,
    uint64_t rootChecksum) const {
    std::array<uint8_t, kProfileLogV2RecordSize> record = {};
    std::copy(kProfileLogV2DescriptorMagic.begin(),
              kProfileLogV2DescriptorMagic.end(),
              record.begin());
    writeProfileLogV2U32(record, 8, kProfileLogV2FormatVersion);
    writeProfileLogV2U32(record, 12, kProfileLogV2RecordSize);
    writeProfileLogV2U64(record, 16, recordGeneration);
    writeProfileLogV2U32(record, 24, rootSlot);
    writeProfileLogV2U64(record, 32, rootChecksum);
    writeProfileLogV2U64(record, 40, profileChecksum);
    writeProfileLogV2U64(record, 48, profileLogV2Checksum(record.data(), 48));
    return record;
  }

  std::optional<ProfileLogV2Descriptor> parseDescriptorRecord(
    const std::array<uint8_t, kProfileLogV2RecordSize>& record) const {
    if (!std::equal(kProfileLogV2DescriptorMagic.begin(),
                    kProfileLogV2DescriptorMagic.end(),
                    record.begin()) ||
        readProfileLogV2U32(record, 8) != kProfileLogV2FormatVersion ||
        readProfileLogV2U32(record, 12) != kProfileLogV2RecordSize ||
        readProfileLogV2U64(record, 40) != profileChecksum ||
        readProfileLogV2U64(record, 48) !=
          profileLogV2Checksum(record.data(), 48) ||
        !profileLogV2HasZeroTail(record, 56)) {
      return std::nullopt;
    }
    uint64_t recordGeneration = readProfileLogV2U64(record, 16);
    uint32_t rootSlot = readProfileLogV2U32(record, 24);
    if (!recordGeneration || rootSlot > 1) {
      return std::nullopt;
    }
    return ProfileLogV2Descriptor{recordGeneration,
                                  rootSlot,
                                  readProfileLogV2U64(record, 32),
                                  readProfileLogV2U64(record, 48)};
  }

  std::array<uint8_t, kProfileLogV2RecordSize> makePhaseRecord(
    uint64_t recordGeneration,
    uint32_t rootSlot,
    uint64_t descriptorChecksum) const {
    std::array<uint8_t, kProfileLogV2RecordSize> record = {};
    std::copy(kProfileLogV2PhaseMagic.begin(),
              kProfileLogV2PhaseMagic.end(),
              record.begin());
    writeProfileLogV2U32(record, 8, kProfileLogV2FormatVersion);
    writeProfileLogV2U32(record, 12, kProfileLogV2RecordSize);
    writeProfileLogV2U64(record, 16, recordGeneration);
    writeProfileLogV2U32(record, 24, rootSlot);
    writeProfileLogV2U32(record, 28, kProfileLogV2PhaseClean);
    writeProfileLogV2U64(record, 32, descriptorChecksum);
    writeProfileLogV2U64(record, 40, profileChecksum);
    writeProfileLogV2U64(record, 48, profileLogV2Checksum(record.data(), 48));
    return record;
  }

  std::optional<ProfileLogV2Phase> parsePhaseRecord(
    const std::array<uint8_t, kProfileLogV2RecordSize>& record) const {
    if (!std::equal(kProfileLogV2PhaseMagic.begin(),
                    kProfileLogV2PhaseMagic.end(),
                    record.begin()) ||
        readProfileLogV2U32(record, 8) != kProfileLogV2FormatVersion ||
        readProfileLogV2U32(record, 12) != kProfileLogV2RecordSize ||
        readProfileLogV2U64(record, 40) != profileChecksum ||
        readProfileLogV2U32(record, 28) != kProfileLogV2PhaseClean ||
        readProfileLogV2U64(record, 48) !=
          profileLogV2Checksum(record.data(), 48) ||
        !profileLogV2HasZeroTail(record, 56)) {
      return std::nullopt;
    }
    uint64_t recordGeneration = readProfileLogV2U64(record, 16);
    uint32_t rootSlot = readProfileLogV2U32(record, 24);
    if (!recordGeneration || rootSlot > 1) {
      return std::nullopt;
    }
    return ProfileLogV2Phase{recordGeneration,
                             rootSlot,
                             readProfileLogV2U64(record, 32)};
  }

  int writeDescriptorPairLocked(uint64_t recordGeneration,
                                uint64_t rootChecksum,
                                uint64_t* descriptorChecksum) {
    auto record = makeDescriptorRecord(
      recordGeneration, recordGeneration & 1, rootChecksum);
    if (descriptorChecksum) {
      *descriptorChecksum = readProfileLogV2U64(record, 48);
    }
    for (uint64_t copy = 0; copy != 2; ++copy) {
      if (int error = writeRecordLocked(
            control, descriptorOffset(recordGeneration, copy), record)) {
        return error;
      }
    }
    return 0;
  }

  int writePhaseWitnessLocked(uint64_t copy,
                              uint64_t recordGeneration,
                              uint32_t rootSlot,
                              uint64_t descriptorChecksum) {
    return writeRecordLocked(
      control,
      phaseOffset(copy),
      makePhaseRecord(recordGeneration, rootSlot, descriptorChecksum));
  }

  int initialiseFreshLocked() {
    // Never recover an incomplete bootstrap by directory inspection or
    // deletion. A missing bootstrap next to any fixed V2 name is ambiguous
    // without durable directory operations and is rejected by the caller.
    if (int error = insertPhysicalFileLocked(rootNames[0])) {
      return error;
    }
    if (int error = insertPhysicalFileLocked(rootNames[1])) {
      return error;
    }
    if (int error = insertPhysicalFileLocked(controlName)) {
      return error;
    }
    if (int error = insertPhysicalFileLocked(bootstrapName)) {
      return error;
    }
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    roots[0] = makePhysicalFile(rootNames[0]);
    roots[1] = makePhysicalFile(rootNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = setFixedSizeLocked(bootstrap, kProfileLogV2BootstrapSize)) {
      return error;
    }
    if (int error = setFixedSizeLocked(control, kProfileLogV2ControlSize)) {
      return error;
    }
    for (const auto& root : roots) {
      if (int error = setFixedSizeLocked(root, kProfileLogV2RootSize)) {
        return error;
      }
    }

    // Generation one lives in slot one. Slot zero is deliberately initialized
    // only to a fixed length; no selector can choose it until generation two
    // has written and flushed a complete root image there.
    const auto root = makeRootRecord(1, 0);
    if (int error = writeRecordLocked(roots[1], 0, root)) {
      return error;
    }
    uint64_t descriptorChecksum = 0;
    if (int error = writeDescriptorPairLocked(
          1, readProfileLogV2U64(root, 40), &descriptorChecksum)) {
      return error;
    }
    if (int error = writePhaseWitnessLocked(0, 1, 1, descriptorChecksum)) {
      return error;
    }
    if (int error = writePhaseWitnessLocked(1, 1, 1, descriptorChecksum)) {
      return error;
    }

    // Bootstrap is deliberately published last and requires two flushed
    // identical witnesses. A partially created file set is never adopted by
    // a later factory; it fails closed rather than pretending direct OPFS
    // directory creation was durable.
    const auto bootstrapRecord = makeBootstrapRecord();
    if (int error = writeRecordLocked(bootstrap, 0, bootstrapRecord)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v2_test_maybe_interrupt(0);
#endif
    if (int error = writeRecordLocked(
          bootstrap, kProfileLogV2RecordSize, bootstrapRecord)) {
      return error;
    }
    generation = 1;
    rootValue = 0;
    return 0;
  }

  int verifyBootstrapLocked() {
    std::array<uint8_t, kProfileLogV2RecordSize> first;
    std::array<uint8_t, kProfileLogV2RecordSize> second;
    if (int error = readRecordLocked(bootstrap, 0, &first)) {
      return error;
    }
    if (int error = readRecordLocked(
          bootstrap, kProfileLogV2RecordSize, &second)) {
      return error;
    }
    // Equal bytes prevent one damaged witness from accidentally being treated
    // as a second endorsement of the same profile identity.
    return parseBootstrapRecord(first) && parseBootstrapRecord(second) &&
           first == second
             ? 0
             : -EIO;
  }

  // Validate one phase witness all the way to its root image. In particular,
  // a g/g+1 phase split is a known pre-quorum state only if *both* the old and
  // new independently duplicated descriptor/root chains are sound. Merely
  // seeing adjacent generation numbers cannot turn a damaged selected control
  // record into a rollback authorization.
  int validatePhaseRootLocked(const ProfileLogV2Phase& phase,
                              ProfileLogV2RootImage* rootImage) {
    if (!rootImage || phase.rootSlot != (phase.generation & 1)) {
      return -EIO;
    }
    std::array<std::optional<ProfileLogV2Descriptor>, 2> descriptors;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV2RecordSize> record;
      if (int error = readRecordLocked(
            control, descriptorOffset(phase.generation, copy), &record)) {
        return error;
      }
      descriptors[copy] = parseDescriptorRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION == 2
    // This fault is applied only after the native parser has successfully
    // read the physical witness. The only permissible outcome is a failed
    // factory; it must not select an older root through damaged control.
    descriptors[0].reset();
#endif
    if (!descriptors[0] || !descriptors[1] ||
        descriptors[0]->generation != phase.generation ||
        descriptors[1]->generation != phase.generation ||
        descriptors[0]->rootSlot != phase.rootSlot ||
        descriptors[1]->rootSlot != phase.rootSlot ||
        descriptors[0]->rootChecksum != descriptors[1]->rootChecksum ||
        descriptors[0]->recordChecksum != phase.descriptorChecksum ||
        descriptors[1]->recordChecksum != phase.descriptorChecksum) {
      return -EIO;
    }
    std::array<uint8_t, kProfileLogV2RecordSize> record;
    if (int error = readRecordLocked(roots[phase.rootSlot], 0, &record)) {
      return error;
    }
    auto root = parseRootRecord(record);
    if (!root || root->generation != phase.generation ||
        root->checksum != descriptors[0]->rootChecksum) {
      return -EIO;
    }
    *rootImage = *root;
    return 0;
  }

  int recoverControlLocked() {
    std::array<std::optional<ProfileLogV2Phase>, 2> phases;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV2RecordSize> record;
      if (int error = readRecordLocked(control, phaseOffset(copy), &record)) {
        return error;
      }
      phases[copy] = parsePhaseRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION == 1
    // The focused test discards one successfully-read selected phase witness.
    // It never mutates OPFS and asserts the native recovery parser fails
    // closed instead of falling back through a damaged selected control state.
    phases[0].reset();
#endif
    if (!phases[0] || !phases[1]) {
      return -EIO;
    }

    ProfileLogV2RootImage selectedRoot = {};
    recoveryNeedsRepair = false;
    if (phases[0]->generation == phases[1]->generation) {
      // A CLEAN quorum has to select one exact descriptor/root chain. Two
      // same-generation witnesses with different targets are corruption, not
      // an invitation to pick either copy.
      if (phases[0]->rootSlot != phases[1]->rootSlot ||
          phases[0]->descriptorChecksum != phases[1]->descriptorChecksum) {
        return -EIO;
      }
      if (int error = validatePhaseRootLocked(*phases[0], &selectedRoot)) {
        return error;
      }
    } else {
      const ProfileLogV2Phase& oldPhase =
        phases[0]->generation < phases[1]->generation ? *phases[0]
                                                       : *phases[1];
      const ProfileLogV2Phase& newPhase =
        phases[0]->generation < phases[1]->generation ? *phases[1]
                                                       : *phases[0];
      // Exactly one new CLEAN witness is a durable pre-quorum state. It may
      // expose the old root only after both chains validate; a gap, slot
      // mismatch, malformed descriptor, or bad root remains fail-closed.
      if (oldPhase.generation == std::numeric_limits<uint64_t>::max() ||
          newPhase.generation != oldPhase.generation + 1 ||
          oldPhase.rootSlot != (oldPhase.generation & 1) ||
          newPhase.rootSlot != (newPhase.generation & 1)) {
        return -EIO;
      }
      if (int error = validatePhaseRootLocked(oldPhase, &selectedRoot)) {
        return error;
      }
      ProfileLogV2RootImage newRoot = {};
      if (int error = validatePhaseRootLocked(newPhase, &newRoot)) {
        return error;
      }
      // Do not reuse the g+1 root slot or descriptors. V2 has no durable
      // rollback/roll-forward decision yet, so this fresh instance is read
      // only until it drains and hands off the profile lease.
      recoveryNeedsRepair = true;
    }
    generation = selectedRoot.generation;
    rootValue = selectedRoot.value;
    return 0;
  }

  int openEstablishedLocked() {
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    roots[0] = makePhysicalFile(rootNames[0]);
    roots[1] = makePhysicalFile(rootNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = requireFixedSizeLocked(
          bootstrap, kProfileLogV2BootstrapSize)) {
      return error;
    }
    if (int error = requireFixedSizeLocked(control, kProfileLogV2ControlSize)) {
      return error;
    }
    for (const auto& root : roots) {
      if (int error = requireFixedSizeLocked(root, kProfileLogV2RootSize)) {
        return error;
      }
    }
    if (int error = verifyBootstrapLocked()) {
      return error;
    }
    return recoverControlLocked();
  }

public:
  // This factory does not expose a mountable root. A caller can only exercise
  // the documented control API, so V2 cannot accidentally become a drop-in
  // direct OPFS profile filesystem while its bounded protocol is under test.
  bool supportsRecordLocks() const override { return false; }
  std::shared_ptr<DataFile> createFile(mode_t) override { return nullptr; }
  std::shared_ptr<Directory> createDirectory(mode_t) override {
    return nullptr;
  }
  std::shared_ptr<Symlink> createSymlink(std::string) override {
    return nullptr;
  }

  int initialise(const char* profileName) {
    std::lock_guard<std::recursive_mutex> lock(controlMutex);
    if (!profileName) {
      return -EINVAL;
    }
    const std::string_view profile(profileName);
    profileChecksum = profileLogV2Checksum(
      reinterpret_cast<const uint8_t*>(profile.data()), profile.size());
    if (profile.size() > std::numeric_limits<uint32_t>::max()) {
      return -EOVERFLOW;
    }
    profileLength = profile.size();
    const std::string stem = storageStem(profile);
    bootstrapName = stem + "-bootstrap";
    controlName = stem + "-control";
    rootNames[0] = stem + "-root-0";
    rootNames[1] = stem + "-root-1";

    if (int error = initialisePhysicalRootLocked()) {
      return error;
    }
    const int bootstrapStatus = lookupPhysicalFileLocked(bootstrapName);
    if (bootstrapStatus == -ENOENT) {
      // Bootstrap does not exist only for a completely absent profile. Do not
      // use directory cleanup to reinterpret an interrupted setup as empty.
      for (const auto& name : {controlName, rootNames[0], rootNames[1]}) {
        if (int status = lookupPhysicalFileLocked(name); status != -ENOENT) {
          return status == 0 ? -EIO : status;
        }
      }
      return initialiseFreshLocked();
    }
    if (bootstrapStatus) {
      return bootstrapStatus;
    }
    for (const auto& name : {controlName, rootNames[0], rootNames[1]}) {
      if (int status = lookupPhysicalFileLocked(name)) {
        return status == -ENOENT ? -EIO : status;
      }
    }
    return openEstablishedLocked();
  }

  int readOPFSProfileLogV2Root(uint64_t* value) override {
    if (!value) {
      return -EINVAL;
    }
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(controlMutex);
    if (fatalError || !generation) {
      return fatalError ? fatalError : -ESHUTDOWN;
    }
    *value = rootValue;
    return 0;
  }

  int commitOPFSProfileLogV2Root(uint64_t value) override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(controlMutex);
    if (fatalError || !generation) {
      return fatalError ? fatalError : -ESHUTDOWN;
    }
    if (recoveryNeedsRepair) {
      // A g/g+1 phase split is readable as the old root, but V2 intentionally
      // has no durable transition that may discard or finish g+1. Refuse to
      // overwrite it until a future protocol supplies such a decision.
      return -ESHUTDOWN;
    }
    if (generation == std::numeric_limits<uint64_t>::max()) {
      return poisonLocked(-EOVERFLOW);
    }
    const uint64_t nextGeneration = generation + 1;
    const uint32_t nextRootSlot = nextGeneration & 1;
    const auto root = makeRootRecord(nextGeneration, value);
    if (int error = writeRecordLocked(roots[nextRootSlot], 0, root)) {
      return poisonLocked(error);
    }
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE == 1
    // Test-only synthetic fault: the root write and flush have returned, then
    // enter the same terminal state used for a lost native acknowledgement.
    // This does not make ProxyWorker::operator() return false; it proves the
    // no-later-proxy response to that state through the explicit failed drain.
    latchProfileLogV2ProxyCompletionForTesting();
    terminalCloseState->recordUnacknowledgedProxyCompletion();
    return poisonLocked(-EIO);
#endif
    uint64_t descriptorChecksum = 0;
    if (int error = writeDescriptorPairLocked(
          nextGeneration,
          readProfileLogV2U64(root, 40),
          &descriptorChecksum)) {
      return poisonLocked(error);
    }

    // The first durable CLEAN witness is intentionally insufficient for a
    // fresh recovery to select |nextGeneration|. It leaves the old phase
    // witness intact, which is the entire old-root-before-quorum proof.
    if (int error = writePhaseWitnessLocked(
          0, nextGeneration, nextRootSlot, descriptorChecksum)) {
      return poisonLocked(error);
    }
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v2_test_maybe_interrupt(1);
#endif
    if (int error = writePhaseWitnessLocked(
          1, nextGeneration, nextRootSlot, descriptorChecksum)) {
      return poisonLocked(error);
    }
#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v2_test_maybe_interrupt(2);
#endif
    generation = nextGeneration;
    rootValue = value;
    return 0;
  }

  int prepareOPFSProfileRetirement(bool checkResources) override {
    int firstError = 0;
    {
      ProfileLeaseState::InternalOperation operation(*profileLeaseState);
      if (!operation) {
        firstError = operation.getError();
      } else {
        std::lock_guard<std::recursive_mutex> lock(controlMutex);
        if (int error = stopAfterUnacknowledgedProxyLocked()) {
          return error;
        }
        // The fixed opaque files retain SyncAccessHandles while live. Close
        // them before the inherited preflight, but do not issue a fresh flush
        // after an earlier protocol failure has already made the control
        // outcome terminally visible.
        firstError = closeAllFilesForRetirementLocked(!fatalError);
        // A close itself can lose completion. Do not fence, preflight, release,
        // or try a second physical close after that latch: the worker becomes
        // a deliberate tombstone and its retained handles die with the
        // document context.
        if (int error = stopAfterUnacknowledgedProxyLocked()) {
          return error;
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

  bool abandonFailedInitialisation() {
    if (initialisationAmbiguous || getOPFSProfilePriorCloseError()) {
      {
        std::lock_guard<std::recursive_mutex> lock(controlMutex);
        abandonPhysicalFilesLocked();
      }
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
    const int preparation = prepareOPFSProfileRetirement(true);
    bool leaseReleased = false;
    const int finish = finishOPFSProfileDrain(
      preparation == 0, &leaseReleased);
    if (!leaseReleased) {
      return false;
    }
    const int retirement = retireOPFSProfileBackend(
      preparation == 0 && finish == 0);
    return preparation == 0 && finish == 0 && retirement == 0;
  }
};

} // anonymous namespace

extern "C" {

#if WASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE == 2
void wasmfs_opfs_direct_operation_admission_race_test_arm(void) {
  armDirectOperationAdmissionRaceForTesting();
}

int wasmfs_opfs_direct_operation_admission_race_test_state(void) {
  return directOperationAdmissionRaceState.load();
}

void wasmfs_opfs_direct_operation_admission_race_test_continue(void) {
  continueDirectOperationAdmissionRaceForTesting();
}

void wasmfs_opfs_direct_operation_admission_race_test_reset(void) {
  resetDirectOperationAdmissionRaceForTesting();
}
#endif

#if WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE
int wasmfs_opfs_profile_log_v2_test_proxies_after_latch(void) {
  return profileLogV2ProxiesAfterLatchForTesting.load();
}
#endif

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

backend_t wasmfs_create_opfs_profile_log_v2_control_backend(
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
  // The fixed-file selector and its cooperative lease need one dedicated
  // worker lifetime. Main-thread Asyncify/JSPI cannot provide that boundary.
  errno = ENOTSUP;
  return NullBackend;
#else
  if (!wasmFS.reserveTerminalLeaseOwner()) {
    errno = EBUSY;
    return NullBackend;
  }
  assert(!emscripten_is_main_browser_thread() &&
         "Cannot safely create leased OPFS backend on main browser thread");

  auto backend = std::make_unique<ProfileLogV2ControlBackend>();
  bool acquireProxyCompleted = false;
  int error = backend->acquireProfileLease(
    profile_name, &acquireProxyCompleted);
  if (error) {
    assert(error < 0);
    if (acquireProxyCompleted) {
      wasmFS.cancelTerminalLeaseOwnerReservation();
    } else {
      wasmFS.markTerminalLeaseOwnerReservationAmbiguous();
      backend->abandonUnacknowledgedProfileLeaseAcquire();
    }
    errno = -error;
    return NullBackend;
  }
  error = backend->initialise(profile_name);
  if (error) {
    assert(error < 0);
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

int wasmfs_opfs_profile_log_v2_read_root(backend_t backend,
                                         uint64_t* value) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    return -operation.getError();
  }
  auto internal = reinterpret_cast<wasmfs::backend_t>(backend);
  if (!value || !wasmFS.ownsBackend(internal)) {
    return -EINVAL;
  }
  if (int error = operation.admitBackend(internal)) {
    return error;
  }
  return internal->readOPFSProfileLogV2Root(value);
}

int wasmfs_opfs_profile_log_v2_commit_root(backend_t backend,
                                           uint64_t value) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    return -operation.getError();
  }
  auto internal = reinterpret_cast<wasmfs::backend_t>(backend);
  if (!wasmFS.ownsBackend(internal)) {
    return -EINVAL;
  }
  if (int error = operation.admitBackend(internal)) {
    return error;
  }
  return internal->commitOPFSProfileLogV2Root(value);
}

void EMSCRIPTEN_KEEPALIVE _wasmfs_opfs_record_entry(
  std::vector<Directory::Entry>* entries, const char* name, int type) {
  entries->push_back({name, File::FileKind(type), 0});
}

} // extern "C"
