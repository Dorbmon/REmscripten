// Copyright 2022 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <emscripten/threading.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <errno.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdlib.h>

#include <set>
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

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE 0
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

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE
// This is the V4 counterpart to the V2 terminal trace. The focused test
// checks the explicit failed drain only; it cannot observe runtime destruction
// after the holder document exits. A selected test must explicitly arm this
// one-shot seam after its setup work. Only the thread that armed it may
// consume it, so an unrelated profile operation on another thread cannot
// consume the fault between the target's arm and mutation. The global state
// still permits only one arm per selected module. Normal builds have neither
// this latch nor these counters or control.
enum ProfileLogV4ProxyCompletionTestState {
  ProfileLogV4ProxyCompletionTestIdle,
  ProfileLogV4ProxyCompletionTestArmed,
  ProfileLogV4ProxyCompletionTestLatched,
};

std::atomic<int> profileLogV4ProxyCompletionStateForTesting =
  ProfileLogV4ProxyCompletionTestIdle;
thread_local bool profileLogV4ProxyCompletionArmedOnThisThread = false;
std::atomic<bool> profileLogV4ProxyCompletionLatchForTesting = false;
std::atomic<uint32_t> profileLogV4ProxyCompletionLatchCountForTesting = 0;
std::atomic<uint32_t> profileLogV4ProxiesAfterLatchForTesting = 0;

int armProfileLogV4ProxyCompletionForTesting() {
  int expected = ProfileLogV4ProxyCompletionTestIdle;
  if (!profileLogV4ProxyCompletionStateForTesting.compare_exchange_strong(
          expected, ProfileLogV4ProxyCompletionTestArmed)) {
    return 0;
  }
  profileLogV4ProxyCompletionArmedOnThisThread = true;
  return 1;
}

bool consumeProfileLogV4ProxyCompletionForTesting() {
  if (!profileLogV4ProxyCompletionArmedOnThisThread) {
    return false;
  }
  profileLogV4ProxyCompletionArmedOnThisThread = false;
  int expected = ProfileLogV4ProxyCompletionTestArmed;
  return profileLogV4ProxyCompletionStateForTesting.compare_exchange_strong(
      expected, ProfileLogV4ProxyCompletionTestLatched);
}

void latchProfileLogV4ProxyCompletionForTesting() {
  profileLogV4ProxyCompletionLatchForTesting.store(true);
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
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE
    if (profileLogV4ProxyCompletionLatchForTesting.load()) {
      ++profileLogV4ProxiesAfterLatchForTesting;
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

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE == 1
  void recordControlledUnacknowledgedProxyCompletionForTesting() {
    // The V4 source-selected publication fault is injected after its browser
    // write and flush have completed. It needs the same no-later-proxy latch
    // as an unknown completion, but it is not a failed browser-handle close
    // that scoped retirement must report as prior cleanup damage.
    int expected = 0;
    (void)unacknowledgedProxyError.compare_exchange_strong(expected, -EIO);
  }
#endif

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

// V3 deliberately remains a one-payload data transaction experiment.  It
// does not expose an OPFS-backed directory tree: callers may attach exactly
// one DataFile through wasmfs_create_file(), which lets the generic paired
// DataFile hooks exercise a real persistent transaction without claiming that
// create/remove/rename have durable namespace semantics.
constexpr size_t kProfileLogV3RecordSize = 128;
constexpr size_t kProfileLogV3BootstrapSize = 2 * kProfileLogV3RecordSize;
constexpr size_t kProfileLogV3ControlSize = 6 * kProfileLogV3RecordSize;
constexpr size_t kProfileLogV3PageSize = 16 * 1024;
constexpr size_t kProfileLogV3DataOffset = 64;
constexpr size_t kProfileLogV3PayloadCapacity =
  kProfileLogV3PageSize - kProfileLogV3DataOffset;
constexpr uint64_t kProfileLogV3MaxSafeOffset = UINT64_C(9007199254740991);
static_assert(kProfileLogV3BootstrapSize <= kProfileLogV3MaxSafeOffset);
static_assert(kProfileLogV3ControlSize <= kProfileLogV3MaxSafeOffset);
static_assert(kProfileLogV3PageSize <= kProfileLogV3MaxSafeOffset);
constexpr uint32_t kProfileLogV3FormatVersion = 3;
constexpr uint32_t kProfileLogV3BootstrapReady = 1;
constexpr uint32_t kProfileLogV3PhaseClean = 1;
constexpr uint32_t kProfileLogV3LayoutEpoch = 1;
constexpr std::array<uint8_t, 8> kProfileLogV3BootstrapMagic = {
  'W', 'F', 'S', 'L', 'G', '3', 'B', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV3DescriptorMagic = {
  'W', 'F', 'S', 'L', 'G', '3', 'D', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV3PhaseMagic = {
  'W', 'F', 'S', 'L', 'G', '3', 'P', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV3ManifestMagic = {
  'W', 'F', 'S', 'L', 'G', '3', 'M', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV3DataMagic = {
  'W', 'F', 'S', 'L', 'G', '3', 'D', 'A'};

#ifndef WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT
#define WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION
#define WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V3_TEST_FORCED_COMMIT_ERROR
#define WASMFS_OPFS_PROFILE_LOG_V3_TEST_FORCED_COMMIT_ERROR 0
#endif

#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT > 1
#error "invalid profile-log V3 interruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION > 3
#error "invalid profile-log V3 selected corruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_FORCED_COMMIT_ERROR < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V3_TEST_FORCED_COMMIT_ERROR > 1
#error "invalid profile-log V3 forced commit error selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT
extern "C" void wasmfs_opfs_profile_log_v3_test_maybe_interrupt(
  int checkpoint);
#endif

uint64_t profileLogV3Checksum(const uint8_t* data, size_t size) {
  uint64_t result = UINT64_C(1469598103934665603);
  for (size_t i = 0; i != size; ++i) {
    result ^= data[i];
    result *= UINT64_C(1099511628211);
  }
  return result;
}

template <size_t Size>
uint64_t profileLogV3ChecksumWithZeroedRange(
  const std::array<uint8_t, Size>& data,
  size_t offset,
  size_t length) {
  if (offset > Size || length > Size - offset) {
    return 0;
  }
  uint64_t result = UINT64_C(1469598103934665603);
  for (size_t i = 0; i != Size; ++i) {
    const uint8_t value = i >= offset && i < offset + length ? 0 : data[i];
    result ^= value;
    result *= UINT64_C(1099511628211);
  }
  return result;
}

template <size_t Size>
bool profileLogV3HasZeroTail(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  return offset <= Size &&
         std::all_of(data.begin() + offset,
                     data.end(),
                     [](uint8_t value) { return value == 0; });
}

template <size_t Size>
bool profileLogV3HasZeroRange(const std::array<uint8_t, Size>& data,
                              size_t offset,
                              size_t length) {
  return offset <= Size && length <= Size - offset &&
         std::all_of(data.begin() + offset,
                     data.begin() + offset + length,
                     [](uint8_t value) { return value == 0; });
}

template <size_t Size>
void writeProfileLogV3U32(std::array<uint8_t, Size>& data,
                          size_t offset,
                          uint32_t value) {
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <size_t Size>
void writeProfileLogV3U64(std::array<uint8_t, Size>& data,
                          size_t offset,
                          uint64_t value) {
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <size_t Size>
uint32_t readProfileLogV3U32(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value |= uint32_t(data[offset + i]) << (8 * i);
  }
  return value;
}

template <size_t Size>
uint64_t readProfileLogV3U64(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value |= uint64_t(data[offset + i]) << (8 * i);
  }
  return value;
}

uint64_t profileLogV3DoubleBits(double value) {
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

double profileLogV3BitsDouble(uint64_t bits) {
  double value = 0;
  static_assert(sizeof(bits) == sizeof(value));
  memcpy(&value, &bits, sizeof(value));
  return value;
}

struct ProfileLogV3Descriptor {
  uint64_t generation;
  uint32_t arena;
  uint64_t manifestOffset;
  uint64_t manifestChecksum;
  uint64_t arenaEnd;
  uint64_t logicalSize;
  uint64_t recordChecksum;
};

struct ProfileLogV3Phase {
  uint64_t generation;
  uint32_t arena;
  uint64_t descriptorChecksum;
};

struct ProfileLogV3Manifest {
  uint64_t generation;
  uint64_t dataOffset;
  uint64_t dataChecksum;
  uint64_t logicalSize;
  File::Metadata metadata;
  uint64_t pageChecksum;
};

struct ProfileLogV3Snapshot {
  std::vector<uint8_t> payload;
  File::Metadata metadata;
};

class ProfileLogV3DataFile;

class ProfileLogV3DataBackend final : public OPFSBackend {
  std::recursive_mutex storeMutex;
  std::shared_ptr<OPFSDirectory> physicalRoot;
  std::shared_ptr<OPFSFile> bootstrap;
  std::shared_ptr<OPFSFile> control;
  std::array<std::shared_ptr<OPFSFile>, 2> arenas;
  bool bootstrapOpen = false;
  bool controlOpen = false;
  std::array<bool, 2> arenasOpen = {};
  bool initialisationAmbiguous = false;
  bool bootstrapComplete = false;
  bool payloadExposed = false;
  bool recoveryNeedsRepair = false;
  uint64_t profileChecksum = 0;
  uint32_t profileLength = 0;
  mode_t payloadMode = 0;
  uint64_t generation = 0;
  ProfileLogV3Snapshot snapshot;
  int fatalError = 0;
  std::string bootstrapName;
  std::string controlName;
  std::array<std::string, 2> arenaNames;

  static std::string storageStem(std::string_view profileName) {
    std::string result = ".wasmfs-profile-log-v3-";
    result += std::to_string(profileName.size());
    result += '-';
    result.append(profileName.data(), profileName.size());
    return result;
  }

  static uint64_t descriptorOffset(uint64_t recordGeneration,
                                   uint64_t copy) {
    return ((recordGeneration & 1) * 2 + copy) * kProfileLogV3RecordSize;
  }

  static uint64_t phaseOffset(uint64_t copy) {
    return (4 + copy) * kProfileLogV3RecordSize;
  }

  static std::optional<uint64_t> alignPage(uint64_t offset) {
    constexpr uint64_t kMask = kProfileLogV3PageSize - 1;
    if (offset > kProfileLogV3MaxSafeOffset - kMask) {
      return std::nullopt;
    }
    return (offset + kMask) & ~kMask;
  }

  static bool validMetadata(const File::Metadata& metadata) {
    return S_ISREG(metadata.mode) && std::isfinite(metadata.atime) &&
           std::isfinite(metadata.mtime) && std::isfinite(metadata.ctime);
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

  // Directory calls are permitted only while the fixed file set is being
  // established or reopened. All steady-state payload operations must use the
  // already-open AccessHandles below.
  int admitBootstrapDirectoryOperationLocked() const {
    return bootstrapComplete ? -ESHUTDOWN : 0;
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
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
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
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
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
    return childType == 1 && childID == 0 ? 0
                                          : childID < 0 ? childID : -EIO;
  }

  int insertPhysicalFileLocked(const std::string& name) {
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
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
    for (size_t i = 0; i != arenas.size(); ++i) {
      if (int error = openFileLocked(arenas[i], &arenasOpen[i])) {
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
    int firstError = 0;
    auto closeOne = [&](const std::shared_ptr<OPFSFile>& file, bool* opened) {
      int error = closeFileForRetirementLocked(file, opened, flushFirst);
      if (!firstError && error) {
        firstError = error;
      }
      return terminalCloseState->getUnacknowledgedProxyError() == 0;
    };
    if (!closeOne(arenas[0], &arenasOpen[0]) ||
        !closeOne(arenas[1], &arenasOpen[1]) ||
        !closeOne(control, &controlOpen) ||
        !closeOne(bootstrap, &bootstrapOpen)) {
      return firstError ? firstError
                        : terminalCloseState->getUnacknowledgedProxyError();
    }
    return firstError;
  }

  void abandonPhysicalFilesLocked() {
    if (bootstrap) {
      bootstrap->abandonForTerminalFailure();
    }
    if (control) {
      control->abandonForTerminalFailure();
    }
    for (const auto& arena : arenas) {
      if (arena) {
        arena->abandonForTerminalFailure();
      }
    }
  }

  int stopAfterUnacknowledgedProxyLocked() {
    const int error = terminalCloseState->getUnacknowledgedProxyError();
    if (!error) {
      return 0;
    }
    abandonPhysicalFilesLocked();
    profileLeaseState->closeDestructorProxying();
    return error;
  }

  int setFixedSizeLocked(const std::shared_ptr<OPFSFile>& file, size_t size) {
    if (!file || size > static_cast<size_t>(std::numeric_limits<off_t>::max())) {
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
                        const std::array<uint8_t, kProfileLogV3RecordSize>&
                          record) {
    if (!file || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    const ssize_t written = file->locked().write(
      record.data(), record.size(), static_cast<off_t>(offset));
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
                       std::array<uint8_t, kProfileLogV3RecordSize>* record) {
    if (!file || !record ||
        offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    const ssize_t read = file->locked().read(
      record->data(), record->size(), static_cast<off_t>(offset));
    if (read < 0) {
      return read;
    }
    return size_t(read) == record->size() ? 0 : -EIO;
  }

  int writePageLocked(const std::shared_ptr<OPFSFile>& file,
                      uint64_t offset,
                      const std::array<uint8_t, kProfileLogV3PageSize>& page) {
    if (!file || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    const ssize_t written = file->locked().write(
      page.data(), page.size(), static_cast<off_t>(offset));
    if (written < 0) {
      return written;
    }
    if (size_t(written) != page.size()) {
      return -EIO;
    }
    return file->locked().flush();
  }

  int readPageLocked(const std::shared_ptr<OPFSFile>& file,
                     uint64_t offset,
                     std::array<uint8_t, kProfileLogV3PageSize>* page) {
    if (!file || !page ||
        offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    const ssize_t read = file->locked().read(
      page->data(), page->size(), static_cast<off_t>(offset));
    if (read < 0) {
      return read;
    }
    return size_t(read) == page->size() ? 0 : -EIO;
  }

  std::array<uint8_t, kProfileLogV3RecordSize> makeBootstrapRecord() const {
    std::array<uint8_t, kProfileLogV3RecordSize> record = {};
    std::copy(kProfileLogV3BootstrapMagic.begin(),
              kProfileLogV3BootstrapMagic.end(),
              record.begin());
    writeProfileLogV3U32(record, 8, kProfileLogV3FormatVersion);
    writeProfileLogV3U32(record, 12, kProfileLogV3RecordSize);
    writeProfileLogV3U64(record, 16, profileChecksum);
    writeProfileLogV3U32(record, 24, profileLength);
    writeProfileLogV3U32(record, 28, static_cast<uint32_t>(payloadMode));
    writeProfileLogV3U32(record, 32, kProfileLogV3PageSize);
    writeProfileLogV3U32(record, 36, arenas.size());
    writeProfileLogV3U32(record, 40, kProfileLogV3BootstrapReady);
    writeProfileLogV3U64(record, 48, kProfileLogV3LayoutEpoch);
    writeProfileLogV3U64(record, 56, profileLogV3Checksum(record.data(), 56));
    return record;
  }

  bool parseBootstrapRecord(
    const std::array<uint8_t, kProfileLogV3RecordSize>& record) const {
    return std::equal(kProfileLogV3BootstrapMagic.begin(),
                      kProfileLogV3BootstrapMagic.end(),
                      record.begin()) &&
           readProfileLogV3U32(record, 8) == kProfileLogV3FormatVersion &&
           readProfileLogV3U32(record, 12) == kProfileLogV3RecordSize &&
           readProfileLogV3U64(record, 16) == profileChecksum &&
           readProfileLogV3U32(record, 24) == profileLength &&
           readProfileLogV3U32(record, 28) == payloadMode &&
           readProfileLogV3U32(record, 32) == kProfileLogV3PageSize &&
           readProfileLogV3U32(record, 36) == arenas.size() &&
           readProfileLogV3U32(record, 40) == kProfileLogV3BootstrapReady &&
           profileLogV3HasZeroRange(record, 44, 4) &&
           readProfileLogV3U64(record, 48) == kProfileLogV3LayoutEpoch &&
           readProfileLogV3U64(record, 56) ==
             profileLogV3Checksum(record.data(), 56) &&
           profileLogV3HasZeroTail(record, 64);
  }

  std::array<uint8_t, kProfileLogV3RecordSize> makeDescriptorRecord(
    const ProfileLogV3Descriptor& descriptor) const {
    std::array<uint8_t, kProfileLogV3RecordSize> record = {};
    std::copy(kProfileLogV3DescriptorMagic.begin(),
              kProfileLogV3DescriptorMagic.end(),
              record.begin());
    writeProfileLogV3U32(record, 8, kProfileLogV3FormatVersion);
    writeProfileLogV3U32(record, 12, kProfileLogV3RecordSize);
    writeProfileLogV3U64(record, 16, descriptor.generation);
    writeProfileLogV3U32(record, 24, descriptor.arena);
    writeProfileLogV3U64(record, 32, descriptor.manifestOffset);
    writeProfileLogV3U64(record, 40, descriptor.manifestChecksum);
    writeProfileLogV3U64(record, 48, descriptor.arenaEnd);
    writeProfileLogV3U64(record, 56, descriptor.logicalSize);
    writeProfileLogV3U64(record, 64, profileChecksum);
    writeProfileLogV3U64(record, 72, kProfileLogV3LayoutEpoch);
    writeProfileLogV3U64(record, 80, profileLogV3Checksum(record.data(), 80));
    return record;
  }

  std::optional<ProfileLogV3Descriptor> parseDescriptorRecord(
    const std::array<uint8_t, kProfileLogV3RecordSize>& record) const {
    if (!std::equal(kProfileLogV3DescriptorMagic.begin(),
                    kProfileLogV3DescriptorMagic.end(),
                    record.begin()) ||
        readProfileLogV3U32(record, 8) != kProfileLogV3FormatVersion ||
        readProfileLogV3U32(record, 12) != kProfileLogV3RecordSize ||
        !profileLogV3HasZeroRange(record, 28, 4) ||
        readProfileLogV3U64(record, 64) != profileChecksum ||
        readProfileLogV3U64(record, 72) != kProfileLogV3LayoutEpoch ||
        readProfileLogV3U64(record, 80) !=
          profileLogV3Checksum(record.data(), 80) ||
        !profileLogV3HasZeroTail(record, 88)) {
      return std::nullopt;
    }
    const uint64_t recordGeneration = readProfileLogV3U64(record, 16);
    const uint32_t arena = readProfileLogV3U32(record, 24);
    const uint64_t logicalSize = readProfileLogV3U64(record, 56);
    if (!recordGeneration || arena >= arenas.size() ||
        logicalSize > kProfileLogV3PayloadCapacity) {
      return std::nullopt;
    }
    return ProfileLogV3Descriptor{recordGeneration,
                                  arena,
                                  readProfileLogV3U64(record, 32),
                                  readProfileLogV3U64(record, 40),
                                  readProfileLogV3U64(record, 48),
                                  logicalSize,
                                  readProfileLogV3U64(record, 80)};
  }

  std::array<uint8_t, kProfileLogV3RecordSize> makePhaseRecord(
    uint64_t recordGeneration,
    uint32_t arena,
    uint64_t descriptorChecksum) const {
    std::array<uint8_t, kProfileLogV3RecordSize> record = {};
    std::copy(kProfileLogV3PhaseMagic.begin(),
              kProfileLogV3PhaseMagic.end(),
              record.begin());
    writeProfileLogV3U32(record, 8, kProfileLogV3FormatVersion);
    writeProfileLogV3U32(record, 12, kProfileLogV3RecordSize);
    writeProfileLogV3U64(record, 16, recordGeneration);
    writeProfileLogV3U32(record, 24, arena);
    writeProfileLogV3U32(record, 28, kProfileLogV3PhaseClean);
    writeProfileLogV3U64(record, 32, descriptorChecksum);
    writeProfileLogV3U64(record, 40, profileChecksum);
    writeProfileLogV3U64(record, 48, profileLogV3Checksum(record.data(), 48));
    return record;
  }

  std::optional<ProfileLogV3Phase> parsePhaseRecord(
    const std::array<uint8_t, kProfileLogV3RecordSize>& record) const {
    if (!std::equal(kProfileLogV3PhaseMagic.begin(),
                    kProfileLogV3PhaseMagic.end(),
                    record.begin()) ||
        readProfileLogV3U32(record, 8) != kProfileLogV3FormatVersion ||
        readProfileLogV3U32(record, 12) != kProfileLogV3RecordSize ||
        readProfileLogV3U64(record, 40) != profileChecksum ||
        readProfileLogV3U32(record, 28) != kProfileLogV3PhaseClean ||
        readProfileLogV3U64(record, 48) !=
          profileLogV3Checksum(record.data(), 48) ||
        !profileLogV3HasZeroTail(record, 56)) {
      return std::nullopt;
    }
    const uint64_t recordGeneration = readProfileLogV3U64(record, 16);
    const uint32_t arena = readProfileLogV3U32(record, 24);
    if (!recordGeneration || arena >= arenas.size()) {
      return std::nullopt;
    }
    return ProfileLogV3Phase{recordGeneration,
                             arena,
                             readProfileLogV3U64(record, 32)};
  }

  std::array<uint8_t, kProfileLogV3PageSize> makeDataPage(
    uint64_t recordGeneration,
    const std::vector<uint8_t>& payload) const {
    std::array<uint8_t, kProfileLogV3PageSize> page = {};
    std::copy(kProfileLogV3DataMagic.begin(),
              kProfileLogV3DataMagic.end(),
              page.begin());
    writeProfileLogV3U32(page, 8, kProfileLogV3FormatVersion);
    writeProfileLogV3U32(page, 12, kProfileLogV3PageSize);
    writeProfileLogV3U64(page, 16, recordGeneration);
    writeProfileLogV3U64(page, 24, payload.size());
    writeProfileLogV3U64(page, 32, profileChecksum);
    std::copy(payload.begin(), payload.end(), page.begin() + kProfileLogV3DataOffset);
    writeProfileLogV3U64(
      page,
      40,
      profileLogV3ChecksumWithZeroedRange(page, 40, sizeof(uint64_t)));
    return page;
  }

  int parseDataPage(const std::array<uint8_t, kProfileLogV3PageSize>& page,
                    uint64_t expectedGeneration,
                    ProfileLogV3Snapshot* result,
                    uint64_t* checksum) const {
    const uint64_t size = readProfileLogV3U64(page, 24);
    if (!result || !checksum ||
        !std::equal(kProfileLogV3DataMagic.begin(),
                    kProfileLogV3DataMagic.end(),
                    page.begin()) ||
        readProfileLogV3U32(page, 8) != kProfileLogV3FormatVersion ||
        readProfileLogV3U32(page, 12) != kProfileLogV3PageSize ||
        readProfileLogV3U64(page, 16) != expectedGeneration ||
        readProfileLogV3U64(page, 32) != profileChecksum ||
        readProfileLogV3U64(page, 40) !=
          profileLogV3ChecksumWithZeroedRange(page, 40, sizeof(uint64_t)) ||
        !profileLogV3HasZeroRange(
          page, 48, kProfileLogV3DataOffset - 48) ||
        size > kProfileLogV3PayloadCapacity ||
        !profileLogV3HasZeroTail(page, kProfileLogV3DataOffset + size)) {
      return -EIO;
    }
    result->payload.assign(page.begin() + kProfileLogV3DataOffset,
                           page.begin() + kProfileLogV3DataOffset + size);
    *checksum = readProfileLogV3U64(page, 40);
    return 0;
  }

  std::array<uint8_t, kProfileLogV3PageSize> makeManifestPage(
    uint64_t recordGeneration,
    uint64_t dataOffset,
    uint64_t dataChecksum,
    const ProfileLogV3Snapshot& next) const {
    std::array<uint8_t, kProfileLogV3PageSize> page = {};
    std::copy(kProfileLogV3ManifestMagic.begin(),
              kProfileLogV3ManifestMagic.end(),
              page.begin());
    writeProfileLogV3U32(page, 8, kProfileLogV3FormatVersion);
    writeProfileLogV3U32(page, 12, kProfileLogV3PageSize);
    writeProfileLogV3U64(page, 16, recordGeneration);
    writeProfileLogV3U64(page, 24, dataOffset);
    writeProfileLogV3U64(page, 32, dataChecksum);
    writeProfileLogV3U64(page, 40, next.payload.size());
    writeProfileLogV3U32(page, 48, next.metadata.mode);
    writeProfileLogV3U64(page, 56, profileLogV3DoubleBits(next.metadata.atime));
    writeProfileLogV3U64(page, 64, profileLogV3DoubleBits(next.metadata.mtime));
    writeProfileLogV3U64(page, 72, profileLogV3DoubleBits(next.metadata.ctime));
    writeProfileLogV3U64(page, 80, profileChecksum);
    writeProfileLogV3U64(
      page,
      88,
      profileLogV3ChecksumWithZeroedRange(page, 88, sizeof(uint64_t)));
    return page;
  }

  std::optional<ProfileLogV3Manifest> parseManifestPage(
    const std::array<uint8_t, kProfileLogV3PageSize>& page) const {
    if (!std::equal(kProfileLogV3ManifestMagic.begin(),
                    kProfileLogV3ManifestMagic.end(),
                    page.begin()) ||
        readProfileLogV3U32(page, 8) != kProfileLogV3FormatVersion ||
        readProfileLogV3U32(page, 12) != kProfileLogV3PageSize ||
        !profileLogV3HasZeroRange(page, 52, 4) ||
        readProfileLogV3U64(page, 80) != profileChecksum ||
        readProfileLogV3U64(page, 88) !=
          profileLogV3ChecksumWithZeroedRange(page, 88, sizeof(uint64_t)) ||
        !profileLogV3HasZeroTail(page, 96)) {
      return std::nullopt;
    }
    const uint64_t logicalSize = readProfileLogV3U64(page, 40);
    File::Metadata metadata = {
      static_cast<mode_t>(readProfileLogV3U32(page, 48)),
      profileLogV3BitsDouble(readProfileLogV3U64(page, 56)),
      profileLogV3BitsDouble(readProfileLogV3U64(page, 64)),
      profileLogV3BitsDouble(readProfileLogV3U64(page, 72)),
    };
    if (!readProfileLogV3U64(page, 16) ||
        logicalSize > kProfileLogV3PayloadCapacity ||
        !validMetadata(metadata)) {
      return std::nullopt;
    }
    return ProfileLogV3Manifest{readProfileLogV3U64(page, 16),
                                readProfileLogV3U64(page, 24),
                                readProfileLogV3U64(page, 32),
                                logicalSize,
                                metadata,
                                readProfileLogV3U64(page, 88)};
  }

  int writeDescriptorPairLocked(const ProfileLogV3Descriptor& descriptor,
                                uint64_t* descriptorChecksum) {
    const auto record = makeDescriptorRecord(descriptor);
    if (descriptorChecksum) {
      *descriptorChecksum = readProfileLogV3U64(record, 80);
    }
    for (uint64_t copy = 0; copy != 2; ++copy) {
      if (int error = writeRecordLocked(
            control, descriptorOffset(descriptor.generation, copy), record)) {
        return error;
      }
    }
    return 0;
  }

  int writePhaseWitnessLocked(uint64_t copy,
                              uint64_t recordGeneration,
                              uint32_t arena,
                              uint64_t descriptorChecksum) {
    return writeRecordLocked(control,
                             phaseOffset(copy),
                             makePhaseRecord(recordGeneration,
                                             arena,
                                             descriptorChecksum));
  }

  int writeGenerationLocked(uint64_t recordGeneration,
                            const ProfileLogV3Snapshot& next) {
    if (!recordGeneration || !validMetadata(next.metadata) ||
        next.payload.size() > kProfileLogV3PayloadCapacity) {
      return -EINVAL;
    }
    const uint32_t arena = recordGeneration & 1;
    const off_t size = arenas[arena]->locked().getSize();
    if (size < 0) {
      return size;
    }
    auto dataOffset = alignPage(static_cast<uint64_t>(size));
    if (!dataOffset || *dataOffset > kProfileLogV3MaxSafeOffset -
                                     2 * kProfileLogV3PageSize) {
      return -EFBIG;
    }
    const uint64_t manifestOffset = *dataOffset + kProfileLogV3PageSize;
    const uint64_t arenaEnd = manifestOffset + kProfileLogV3PageSize;
    const auto data = makeDataPage(recordGeneration, next.payload);
    const uint64_t dataChecksum = readProfileLogV3U64(data, 40);
    const auto manifest = makeManifestPage(
      recordGeneration, *dataOffset, dataChecksum, next);
    const uint64_t manifestChecksum = readProfileLogV3U64(manifest, 88);
    if (int error = writePageLocked(arenas[arena], *dataOffset, data)) {
      return error;
    }
    if (int error = writePageLocked(arenas[arena], manifestOffset, manifest)) {
      return error;
    }
    const ProfileLogV3Descriptor descriptor = {recordGeneration,
                                                arena,
                                                manifestOffset,
                                                manifestChecksum,
                                                arenaEnd,
                                                next.payload.size(),
                                                0};
    uint64_t descriptorChecksum = 0;
    if (int error = writeDescriptorPairLocked(descriptor, &descriptorChecksum)) {
      return error;
    }
    if (int error = writePhaseWitnessLocked(
          0, recordGeneration, arena, descriptorChecksum)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v3_test_maybe_interrupt(1);
#endif
    if (int error = writePhaseWitnessLocked(
          1, recordGeneration, arena, descriptorChecksum)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v3_test_maybe_interrupt(2);
#endif
    return 0;
  }

  int validatePhaseSnapshotLocked(const ProfileLogV3Phase& phase,
                                  ProfileLogV3Snapshot* result) {
    if (!result || phase.arena != (phase.generation & 1)) {
      return -EIO;
    }
    std::array<std::optional<ProfileLogV3Descriptor>, 2> descriptors;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV3RecordSize> record;
      if (int error = readRecordLocked(
            control, descriptorOffset(phase.generation, copy), &record)) {
        return error;
      }
      descriptors[copy] = parseDescriptorRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION == 2
    descriptors[0].reset();
#endif
    if (!descriptors[0] || !descriptors[1] ||
        descriptors[0]->generation != phase.generation ||
        descriptors[1]->generation != phase.generation ||
        descriptors[0]->arena != phase.arena ||
        descriptors[1]->arena != phase.arena ||
        descriptors[0]->manifestOffset != descriptors[1]->manifestOffset ||
        descriptors[0]->manifestChecksum != descriptors[1]->manifestChecksum ||
        descriptors[0]->arenaEnd != descriptors[1]->arenaEnd ||
        descriptors[0]->logicalSize != descriptors[1]->logicalSize ||
        descriptors[0]->recordChecksum != phase.descriptorChecksum ||
        descriptors[1]->recordChecksum != phase.descriptorChecksum) {
      return -EIO;
    }
    const ProfileLogV3Descriptor& descriptor = *descriptors[0];
    if (descriptor.manifestOffset % kProfileLogV3PageSize ||
        descriptor.manifestOffset > kProfileLogV3MaxSafeOffset -
                                      kProfileLogV3PageSize ||
        descriptor.manifestOffset + kProfileLogV3PageSize !=
          descriptor.arenaEnd) {
      return -EIO;
    }
    const off_t physicalSize = arenas[descriptor.arena]->locked().getSize();
    if (physicalSize < 0) {
      return physicalSize;
    }
    if (static_cast<uint64_t>(physicalSize) < descriptor.arenaEnd) {
      return -EIO;
    }
    std::array<uint8_t, kProfileLogV3PageSize> manifestPage;
    if (int error = readPageLocked(
          arenas[descriptor.arena], descriptor.manifestOffset, &manifestPage)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION == 3
    manifestPage[0] ^= 1;
#endif
    auto manifest = parseManifestPage(manifestPage);
    if (!manifest || manifest->generation != phase.generation ||
        manifest->pageChecksum != descriptor.manifestChecksum ||
        manifest->logicalSize != descriptor.logicalSize ||
        manifest->dataOffset % kProfileLogV3PageSize ||
        manifest->dataOffset > kProfileLogV3MaxSafeOffset -
                                  kProfileLogV3PageSize ||
        manifest->dataOffset + kProfileLogV3PageSize !=
          descriptor.manifestOffset) {
      return -EIO;
    }
    std::array<uint8_t, kProfileLogV3PageSize> dataPage;
    if (int error = readPageLocked(
          arenas[descriptor.arena], manifest->dataOffset, &dataPage)) {
      return error;
    }
    uint64_t dataChecksum = 0;
    if (int error = parseDataPage(
          dataPage, phase.generation, result, &dataChecksum)) {
      return error;
    }
    if (dataChecksum != manifest->dataChecksum ||
        result->payload.size() != manifest->logicalSize) {
      return -EIO;
    }
    result->metadata = manifest->metadata;
    return 0;
  }

  int recoverControlLocked() {
    std::array<std::optional<ProfileLogV3Phase>, 2> phases;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV3RecordSize> record;
      if (int error = readRecordLocked(control, phaseOffset(copy), &record)) {
        return error;
      }
      phases[copy] = parsePhaseRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION == 1
    phases[0].reset();
#endif
    if (!phases[0] || !phases[1]) {
      return -EIO;
    }
    recoveryNeedsRepair = false;
    ProfileLogV3Snapshot selected;
    if (phases[0]->generation == phases[1]->generation) {
      if (phases[0]->arena != phases[1]->arena ||
          phases[0]->descriptorChecksum != phases[1]->descriptorChecksum) {
        return -EIO;
      }
      if (int error = validatePhaseSnapshotLocked(*phases[0], &selected)) {
        return error;
      }
      generation = phases[0]->generation;
    } else {
      const ProfileLogV3Phase& oldPhase =
        phases[0]->generation < phases[1]->generation ? *phases[0]
                                                       : *phases[1];
      const ProfileLogV3Phase& newPhase =
        phases[0]->generation < phases[1]->generation ? *phases[1]
                                                       : *phases[0];
      if (oldPhase.generation == std::numeric_limits<uint64_t>::max() ||
          newPhase.generation != oldPhase.generation + 1 ||
          oldPhase.arena != (oldPhase.generation & 1) ||
          newPhase.arena != (newPhase.generation & 1)) {
        return -EIO;
      }
      if (int error = validatePhaseSnapshotLocked(oldPhase, &selected)) {
        return error;
      }
      ProfileLogV3Snapshot newSnapshot;
      if (int error = validatePhaseSnapshotLocked(newPhase, &newSnapshot)) {
        return error;
      }
      generation = oldPhase.generation;
      recoveryNeedsRepair = true;
    }
    snapshot = std::move(selected);
    return 0;
  }

  int verifyBootstrapLocked() {
    std::array<uint8_t, kProfileLogV3RecordSize> first;
    std::array<uint8_t, kProfileLogV3RecordSize> second;
    if (int error = readRecordLocked(bootstrap, 0, &first)) {
      return error;
    }
    if (int error = readRecordLocked(
          bootstrap, kProfileLogV3RecordSize, &second)) {
      return error;
    }
    return parseBootstrapRecord(first) && parseBootstrapRecord(second) &&
           first == second
             ? 0
             : -EIO;
  }

  int initialiseFreshLocked() {
    for (const auto& name : {arenaNames[0], arenaNames[1], controlName,
                             bootstrapName}) {
      if (int error = insertPhysicalFileLocked(name)) {
        return error;
      }
    }
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    arenas[0] = makePhysicalFile(arenaNames[0]);
    arenas[1] = makePhysicalFile(arenaNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = setFixedSizeLocked(bootstrap, kProfileLogV3BootstrapSize)) {
      return error;
    }
    if (int error = setFixedSizeLocked(control, kProfileLogV3ControlSize)) {
      return error;
    }
    const double now = emscripten_date_now();
    ProfileLogV3Snapshot initial = {
      {}, {static_cast<mode_t>(S_IFREG | payloadMode), now, now, now}};
    if (int error = writeGenerationLocked(1, initial)) {
      return error;
    }
    const auto bootstrapRecord = makeBootstrapRecord();
    if (int error = writeRecordLocked(bootstrap, 0, bootstrapRecord)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v3_test_maybe_interrupt(0);
#endif
    if (int error = writeRecordLocked(
          bootstrap, kProfileLogV3RecordSize, bootstrapRecord)) {
      return error;
    }
    generation = 1;
    snapshot = std::move(initial);
    return 0;
  }

  int openEstablishedLocked() {
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    arenas[0] = makePhysicalFile(arenaNames[0]);
    arenas[1] = makePhysicalFile(arenaNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = requireFixedSizeLocked(
          bootstrap, kProfileLogV3BootstrapSize)) {
      return error;
    }
    if (int error = requireFixedSizeLocked(control, kProfileLogV3ControlSize)) {
      return error;
    }
    if (int error = verifyBootstrapLocked()) {
      return error;
    }
    return recoverControlLocked();
  }

  int commitSnapshotLocked(const ProfileLogV3Snapshot& next) {
    if (fatalError || !generation) {
      return fatalError ? fatalError : -ESHUTDOWN;
    }
    if (recoveryNeedsRepair) {
      return -ESHUTDOWN;
    }
    if (generation == std::numeric_limits<uint64_t>::max()) {
      return poisonLocked(-EOVERFLOW);
    }
    if (!validMetadata(next.metadata) ||
        next.payload.size() > kProfileLogV3PayloadCapacity) {
      return -EINVAL;
    }
#if WASMFS_OPFS_PROFILE_LOG_V3_TEST_FORCED_COMMIT_ERROR
    // This is a synthetic pre-write test fault, not an OPFS crash model. It
    // makes the first candidate transaction terminally fail so the focused
    // test can prove that the already-attached DataFile does not later reopen
    // through a backend that has latched an indeterminate commit outcome.
    return poisonLocked(-EIO);
#endif
    if (int error = writeGenerationLocked(generation + 1, next)) {
      return poisonLocked(error);
    }
    generation++;
    snapshot = next;
    return 0;
  }

  int payloadOperationErrorLocked() const {
    return fatalError ? fatalError : generation ? 0 : -ESHUTDOWN;
  }

  friend class ProfileLogV3DataFile;

public:
  bool supportsExplicitMetadataMutation() const override { return true; }
  bool requiresAtomicMetadataMutations() const override { return true; }
  bool supportsRecordLocks() const override { return false; }

  // V3 deliberately has no persistent directory. A caller may only attach
  // its one DataFile with wasmfs_create_file() into an existing namespace.
  std::shared_ptr<Directory> createDirectory(mode_t) override { return nullptr; }
  std::shared_ptr<Symlink> createSymlink(std::string) override {
    return nullptr;
  }
  std::shared_ptr<DataFile> createFile(mode_t mode) override;

  int initialise(const char* profileName, mode_t requestedPayloadMode) {
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (!profileName || (requestedPayloadMode & ~S_IALLUGO) != 0) {
      return -EINVAL;
    }
    const std::string_view profile(profileName);
    if (profile.size() > std::numeric_limits<uint32_t>::max()) {
      return -EOVERFLOW;
    }
    profileChecksum = profileLogV3Checksum(
      reinterpret_cast<const uint8_t*>(profile.data()), profile.size());
    profileLength = profile.size();
    payloadMode = requestedPayloadMode;
    const std::string stem = storageStem(profile);
    bootstrapName = stem + "-bootstrap";
    controlName = stem + "-control";
    arenaNames[0] = stem + "-arena-0";
    arenaNames[1] = stem + "-arena-1";

    if (int error = initialisePhysicalRootLocked()) {
      return error;
    }
    const int bootstrapStatus = lookupPhysicalFileLocked(bootstrapName);
    if (bootstrapStatus == -ENOENT) {
      for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
        if (int status = lookupPhysicalFileLocked(name); status != -ENOENT) {
          return status == 0 ? -EIO : status;
        }
      }
      if (int error = initialiseFreshLocked()) {
        return error;
      }
    } else {
      if (bootstrapStatus) {
        return bootstrapStatus;
      }
      for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
        if (int status = lookupPhysicalFileLocked(name)) {
          return status == -ENOENT ? -EIO : status;
        }
      }
      if (int error = openEstablishedLocked()) {
        return error;
      }
    }
    // From this point onward every V3 operation has all four OPFS files open.
    // Any attempted directory lookup/creation is rejected by the guard above.
    bootstrapComplete = true;
    return 0;
  }

  off_t getPayloadSize() {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = payloadOperationErrorLocked()) {
      return error;
    }
    return snapshot.payload.size();
  }

  int openPayload() {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    // A valid g/g+1 recovery remains attachable for read-only access. Only a
    // terminal backend error (or absent initialized snapshot) rejects the
    // descriptor open.
    return payloadOperationErrorLocked();
  }

  ssize_t readPayload(uint8_t* buffer, size_t length, off_t offset) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = payloadOperationErrorLocked()) {
      return error;
    }
    if (offset < 0) {
      return -EINVAL;
    }
    if (static_cast<uint64_t>(offset) >= snapshot.payload.size()) {
      return 0;
    }
    const size_t start = offset;
    const size_t count = std::min(length, snapshot.payload.size() - start);
    std::copy_n(snapshot.payload.data() + start, count, buffer);
    return count;
  }

  ssize_t writePayloadWithMetadata(const uint8_t* buffer,
                                   size_t length,
                                   off_t offset,
                                   const File::Metadata& metadata) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = payloadOperationErrorLocked()) {
      return error;
    }
    if (offset < 0 || !validMetadata(metadata)) {
      return -EINVAL;
    }
    const uint64_t start = offset;
    if (start > kProfileLogV3PayloadCapacity ||
        length > kProfileLogV3PayloadCapacity - start) {
      return -EFBIG;
    }
    if (!length) {
      return 0;
    }
    ProfileLogV3Snapshot next = snapshot;
    const size_t end = start + length;
    if (end > next.payload.size()) {
      next.payload.resize(end);
    }
    std::copy_n(buffer, length, next.payload.data() + start);
    next.metadata = metadata;
    if (int error = commitSnapshotLocked(next)) {
      return error;
    }
    return length;
  }

  int resizePayloadWithMetadata(off_t size, const File::Metadata& metadata) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = payloadOperationErrorLocked()) {
      return error;
    }
    if (size < 0 || static_cast<uint64_t>(size) > kProfileLogV3PayloadCapacity ||
        !validMetadata(metadata)) {
      return -EINVAL;
    }
    ProfileLogV3Snapshot next = snapshot;
    next.payload.resize(size);
    next.metadata = metadata;
    return commitSnapshotLocked(next);
  }

  int persistPayloadMetadata(const File::Metadata& metadata) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = payloadOperationErrorLocked()) {
      return error;
    }
    if (!validMetadata(metadata)) {
      return -EINVAL;
    }
    ProfileLogV3Snapshot next = snapshot;
    next.metadata = metadata;
    return commitSnapshotLocked(next);
  }

  int flushPayload() {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = payloadOperationErrorLocked()) {
      return error;
    }
    if (int error = arenas[generation & 1]->locked().flush()) {
      return poisonLocked(error);
    }
    if (int error = control->locked().flush()) {
      return poisonLocked(error);
    }
    return 0;
  }

  int prepareOPFSProfileRetirement(bool checkResources) override {
    int firstError = 0;
    {
      ProfileLeaseState::InternalOperation operation(*profileLeaseState);
      if (!operation) {
        firstError = operation.getError();
      } else {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        if (int error = stopAfterUnacknowledgedProxyLocked()) {
          return error;
        }
        firstError = closeAllFilesForRetirementLocked(!fatalError);
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
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
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

class ProfileLogV3DataFile final : public DataFile {
  ProfileLogV3DataBackend* backend;

  off_t getSize() override { return backend->getPayloadSize(); }
  // The generic WasmFS open path validates the flags and owns O_TRUNC and
  // O_APPEND behavior. This file is already backed by the V3 backend's fixed
  // open arena handles, so a descriptor open itself has no additional OPFS
  // operation. It still must recheck the backend terminal latch before the
  // O_CREAT|O_EXCL|O_RDWR attach or any later open of the mounted path.
  int open(oflags_t) override { return backend->openPayload(); }
  int close() override { return 0; }
  ssize_t read(uint8_t* buffer, size_t length, off_t offset) override {
    return backend->readPayload(buffer, length, offset);
  }
  ssize_t write(const uint8_t*, size_t, off_t) override {
    // The backend opts into the paired hook. Any accidental legacy call must
    // fail rather than publish data with stale metadata.
    return -ENOTSUP;
  }
  ssize_t writeWithMetadata(const uint8_t* buffer,
                            size_t length,
                            off_t offset,
                            const Metadata& metadata) override {
    return backend->writePayloadWithMetadata(buffer, length, offset, metadata);
  }
  int setSize(off_t) override { return -ENOTSUP; }
  int setSizeWithMetadata(off_t size, const Metadata& metadata) override {
    return backend->resizePayloadWithMetadata(size, metadata);
  }
  int flush() override { return backend->flushPayload(); }
  int persistMetadata(const Metadata& metadata) override {
    return backend->persistPayloadMetadata(metadata);
  }

public:
  ProfileLogV3DataFile(ProfileLogV3DataBackend* backend,
                       const Metadata& metadata)
    : DataFile(metadata.mode & ~S_IFMT, backend), backend(backend) {
    mode = metadata.mode;
    atime = metadata.atime;
    mtime = metadata.mtime;
    ctime = metadata.ctime;
  }
};

std::shared_ptr<DataFile> ProfileLogV3DataBackend::createFile(mode_t mode) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(storeMutex);
  if (fatalError || payloadExposed ||
      (mode & (S_IRWXUGO | S_ISVTX)) != payloadMode) {
    return nullptr;
  }
  payloadExposed = true;
  return std::make_shared<ProfileLogV3DataFile>(this, snapshot.metadata);
}

// V4 is deliberately a non-mountable foundation for the scalable profile
// filesystem. Unlike V3's fixed 16 KiB payload page, it commits an arbitrary
// length immutable manifest record in one of two append-only arenas. The
// descriptor records both arena high-water marks so a later inode/extent
// implementation can reference immutable records from either arena without
// relying on OPFS directory durability.
constexpr size_t kProfileLogV4RecordSize = 128;
constexpr size_t kProfileLogV4BootstrapSize = 2 * kProfileLogV4RecordSize;
constexpr size_t kProfileLogV4ControlSize = 6 * kProfileLogV4RecordSize;
constexpr size_t kProfileLogV4ManifestHeaderSize = 96;
// Recovery must validate arbitrary manifest sizes without consuming a large
// fraction of an Emscripten application worker's stack.
constexpr size_t kProfileLogV4TransferSize = 16 * 1024;
constexpr uint64_t kProfileLogV4MaxSafeOffset = UINT64_C(9007199254740991);
constexpr uint32_t kProfileLogV4FormatVersion = 4;
constexpr uint32_t kProfileLogV4BootstrapReady = 1;
// The mountable filesystem uses a durable PREPARED bootstrap witness before
// it creates the remaining fixed physical files. The opaque V4 experiment
// retains its original READY-only bootstrap protocol for compatibility with
// its deliberately fail-closed recovery boundary.
constexpr uint32_t kProfileLogV4BootstrapPrepared = 2;
constexpr uint64_t kProfileLogV4BootstrapPreparedGeneration = 1;
constexpr uint64_t kProfileLogV4BootstrapReadyFirstGeneration = 2;
constexpr uint64_t kProfileLogV4BootstrapReadySecondGeneration = 3;
constexpr uint32_t kProfileLogV4PhaseClean = 1;
constexpr uint32_t kProfileLogV4LayoutEpoch = 1;
constexpr std::array<uint8_t, 8> kProfileLogV4BootstrapMagic = {
  'W', 'F', 'S', 'L', 'G', '4', 'B', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV4DescriptorMagic = {
  'W', 'F', 'S', 'L', 'G', '4', 'D', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV4PhaseMagic = {
  'W', 'F', 'S', 'L', 'G', '4', 'P', '0'};
constexpr std::array<uint8_t, 8> kProfileLogV4ManifestMagic = {
  'W', 'F', 'S', 'L', 'G', '4', 'M', '0'};

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_PARENT_CORRUPTION
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_PARENT_CORRUPTION 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_EXTENT_CORRUPTION
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_EXTENT_CORRUPTION 0
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST 0
#endif

// Test-only override for the V4 filesystem checkpoint cadence. Zero keeps
// the production cadence; an odd value of at least three lets focused browser
// tests exercise alternating two-arena reclamation without waiting for a full
// production interval. This controls system-library source selection only.
#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL 0
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT > 1
#error "invalid profile-log V4 interruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION > 4
#error "invalid profile-log V4 selected corruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE > 1
#error "invalid profile-log V4 proxy completion failure selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION > 2
#error "invalid profile-log V4 live corruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_PARENT_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_PARENT_CORRUPTION > 1
#error "invalid profile-log V4 historical parent corruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_EXTENT_CORRUPTION < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_EXTENT_CORRUPTION > 1
#error "invalid profile-log V4 historical extent corruption selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST < 0 || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST > 1
#error "invalid profile-log V4 empty post-root manifest selector"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL < 0 || \
  (WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL && \
   WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL < 3) || \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL > 31 || \
  (WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL && \
   !(WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL & 1))
#error "invalid profile-log V4 filesystem checkpoint interval"
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
extern "C" void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(
  int checkpoint);
#endif

static_assert(kProfileLogV4BootstrapSize <= kProfileLogV4MaxSafeOffset);
static_assert(kProfileLogV4ControlSize <= kProfileLogV4MaxSafeOffset);
static_assert(kProfileLogV4ManifestHeaderSize <= kProfileLogV4MaxSafeOffset);

uint64_t profileLogV4ChecksumUpdate(uint64_t result,
                                    const uint8_t* data,
                                    size_t size) {
  for (size_t i = 0; i != size; ++i) {
    result ^= data[i];
    result *= UINT64_C(1099511628211);
  }
  return result;
}

uint64_t profileLogV4Checksum(const uint8_t* data, size_t size) {
  return profileLogV4ChecksumUpdate(UINT64_C(1469598103934665603), data, size);
}

template <size_t Size>
uint64_t profileLogV4ChecksumWithZeroedRange(
  const std::array<uint8_t, Size>& data,
  size_t offset,
  size_t length) {
  if (offset > Size || length > Size - offset) {
    return 0;
  }
  uint64_t result = UINT64_C(1469598103934665603);
  for (size_t i = 0; i != Size; ++i) {
    const uint8_t value = i >= offset && i < offset + length ? 0 : data[i];
    result ^= value;
    result *= UINT64_C(1099511628211);
  }
  return result;
}

template <size_t Size>
bool profileLogV4HasZeroTail(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  return offset <= Size &&
         std::all_of(data.begin() + offset,
                     data.end(),
                     [](uint8_t value) { return value == 0; });
}

template <size_t Size>
bool profileLogV4HasZeroRange(const std::array<uint8_t, Size>& data,
                              size_t offset,
                              size_t length) {
  return offset <= Size && length <= Size - offset &&
         std::all_of(data.begin() + offset,
                     data.begin() + offset + length,
                     [](uint8_t value) { return value == 0; });
}

template <size_t Size>
void writeProfileLogV4U32(std::array<uint8_t, Size>& data,
                          size_t offset,
                          uint32_t value) {
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <size_t Size>
void writeProfileLogV4U64(std::array<uint8_t, Size>& data,
                          size_t offset,
                          uint64_t value) {
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

template <size_t Size>
uint32_t readProfileLogV4U32(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value |= uint32_t(data[offset + i]) << (8 * i);
  }
  return value;
}

template <size_t Size>
uint64_t readProfileLogV4U64(const std::array<uint8_t, Size>& data,
                             size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i != sizeof(value); ++i) {
    value |= uint64_t(data[offset + i]) << (8 * i);
  }
  return value;
}

struct ProfileLogV4Descriptor {
  uint64_t generation;
  uint32_t arena;
  uint64_t manifestOffset;
  uint64_t manifestSize;
  uint64_t manifestRecordChecksum;
  std::array<uint64_t, 2> highWater;
  uint64_t recordChecksum;
};

// A logical delta names its immediately preceding sealed outer manifest by
// this exact, self-authenticating locator. Keep it smaller than a descriptor:
// the selected descriptor remains the sole authority for the high-water
// bounds that make a historical record reachable.
struct ProfileLogV4ManifestReference {
  uint64_t generation;
  uint32_t arena;
  uint64_t manifestOffset;
  uint64_t manifestSize;
  uint64_t manifestRecordChecksum;
};

ProfileLogV4ManifestReference profileLogV4ManifestReference(
  const ProfileLogV4Descriptor& descriptor) {
  return {descriptor.generation,
          descriptor.arena,
          descriptor.manifestOffset,
          descriptor.manifestSize,
          descriptor.manifestRecordChecksum};
}

struct ProfileLogV4Phase {
  uint64_t generation;
  uint32_t arena;
  uint64_t descriptorChecksum;
};

enum class ProfileLogV4BootstrapState : uint32_t {
  Ready = kProfileLogV4BootstrapReady,
  Prepared = kProfileLogV4BootstrapPrepared,
};

struct ProfileLogV4BootstrapRecord {
  ProfileLogV4BootstrapState state;
  uint64_t generation;
  bool legacy;
};

enum class ProfileLogV4BootstrapDisposition {
  Established,
  Prepared,
  InitialGeneration,
  Empty,
  Invalid,
};

struct ProfileLogV4ManifestHeader {
  uint64_t generation;
  uint64_t size;
  uint64_t payloadChecksum;
  uint64_t recordChecksum;
};

bool profileLogV4SameDescriptor(const ProfileLogV4Descriptor& lhs,
                                const ProfileLogV4Descriptor& rhs) {
  return lhs.generation == rhs.generation && lhs.arena == rhs.arena &&
         lhs.manifestOffset == rhs.manifestOffset &&
         lhs.manifestSize == rhs.manifestSize &&
         lhs.manifestRecordChecksum == rhs.manifestRecordChecksum &&
         lhs.highWater == rhs.highWater &&
         lhs.recordChecksum == rhs.recordChecksum;
}

// Own the fixed-file V4 envelope independently from the logical payload that
// uses it.  The opaque manifest experiment below deliberately exposes only
// this store, while the future profile filesystem composes its inode/extent
// transaction on top of the exact same bootstrap, selector, and retirement
// protocol.  Keep the store private to this translation unit: callers must
// never be able to bypass their logical-format validation through this layer.
class ProfileLogV4FilesystemBackend;
class ProfileLogV4Store : public OPFSBackend {
protected:
  // A transaction reserves the next selected generation and its parity arena
  // while ProfileLogV4Store holds its selector lock. Callers can append
  // immutable records before producing the manifest that names them; the
  // manifest remains unreachable until the existing descriptor and witness
  // quorum is published.  It is intentionally available only to logical
  // payload backends in this translation unit.
  class Transaction {
    ProfileLogV4Store& store;
    uint64_t nextGeneration;
    uint32_t nextArena;
    std::array<uint64_t, 2> nextHighWater;
    ProfileLogV4ManifestReference parent;
    bool discardInactiveArena = false;

    friend class ProfileLogV4Store;
    friend class ProfileLogV4FilesystemBackend;

    Transaction(ProfileLogV4Store& store,
                uint64_t generation,
                std::array<uint64_t, 2> highWater,
                ProfileLogV4ManifestReference parent)
      : store(store),
        nextGeneration(generation),
        nextArena(generation & 1),
        nextHighWater(highWater),
        parent(parent) {}

  public:
    uint64_t generation() const { return nextGeneration; }
    uint32_t arena() const { return nextArena; }
    const ProfileLogV4ManifestReference& parentReference() const {
      return parent;
    }

  private:
    // The caller has copied every live reference into |arena()| and may
    // therefore publish a descriptor that no longer names the opposite
    // arena. This must be requested only after every immutable checkpoint
    // record has been appended: the outer store writes the descriptor and
    // both phase witnesses before it can truncate that old arena.
    int discardInactiveArenaAfterPublish() {
      if (discardInactiveArena) {
        return -EIO;
      }
      nextHighWater[nextArena ^ 1] = 0;
      discardInactiveArena = true;
      return 0;
    }

  public:
    // Append one immutable record to the selected parity arena and flush it
    // before returning its physical offset. A later failed selector publish
    // leaves this record unreachable; a retry may safely overwrite the tail
    // because the selected descriptor still carries the old high-water mark.
    int append(const uint8_t* data, size_t size, uint64_t* offset) {
      if (!offset || (size && !data)) {
        return -EINVAL;
      }
      const uint64_t start = nextHighWater[nextArena];
      if (start > kProfileLogV4MaxSafeOffset ||
          size > kProfileLogV4MaxSafeOffset - start ||
          start > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
          size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) -
                   start) {
        return -EFBIG;
      }
      if (size) {
        if (int error = store.writeBytesAndFlushLocked(
              store.arenas[nextArena], start, data, size)) {
          return error;
        }
      }
      *offset = start;
      nextHighWater[nextArena] = start + size;
      return 0;
    }
  };

  using TransactionBuilder =
    std::function<int(Transaction&, std::vector<uint8_t>*)>;

  std::recursive_mutex storeMutex;
  std::shared_ptr<OPFSDirectory> physicalRoot;
  std::shared_ptr<OPFSFile> bootstrap;
  std::shared_ptr<OPFSFile> control;
  std::array<std::shared_ptr<OPFSFile>, 2> arenas;
  bool bootstrapOpen = false;
  bool controlOpen = false;
  std::array<bool, 2> arenasOpen = {};
  bool initialisationAmbiguous = false;
  bool bootstrapComplete = false;
  uint64_t profileChecksum = 0;
  uint32_t profileLength = 0;
  uint64_t generation = 0;
  std::array<uint64_t, 2> highWater = {};
  ProfileLogV4Descriptor selectedDescriptor = {};
  bool hasSelectedDescriptor = false;
  int fatalError = 0;
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE == 1
  // The selected publication fault is injected only after the underlying
  // manifest write and flush have returned. It reuses the terminal no-proxy
  // latch so ordinary post-fault access stays fail-closed, but unlike a real
  // missing ProxyWorker completion it leaves no browser-side handle operation
  // of unknown outcome.
  bool controlledProxyCompletionFailureForTesting = false;

  bool hasControlledProxyCompletionFailureForTesting() {
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    return controlledProxyCompletionFailureForTesting;
  }
#endif
  // An empty tag retains the names published by the V4 opaque-manifest
  // experiment.  A logical filesystem must use a distinct tag so an opaque
  // payload, including V4's initial zero-length manifest, can never be
  // reinterpreted as a mounted profile.
  std::string storageTag;
  // Only the mountable filesystem has an explicit pre-exposure reset
  // protocol. Keep the original opaque manifest primitive fail-closed on a
  // partial first bootstrap so its narrow experiment retains its published
  // recovery contract.
  bool recoverInitialBootstrap = false;
  std::string bootstrapName;
  std::string controlName;
  std::array<std::string, 2> arenaNames;

  int removeRecoverableBootstrapPhysicalFileLocked(const std::string& name) {
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
    DirectOPFSOperation operation(*profileLeaseState, terminalCloseState);
    if (!operation) {
      return operation.getError();
    }
    // _wasmfs_opfs_remove_child leaves its out parameter untouched on
    // success, so success must start at zero rather than an error sentinel.
    int error = 0;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_remove_child(
            ctx.ctx, kOPFSRootDirectoryID, name.c_str(), &error);
        })) {
      return recordInitialisationProxyFailure();
    }
    return error;
  }

  std::string storageStem(std::string_view profileName) const {
    std::string result = ".wasmfs-profile-log-v4";
    if (!storageTag.empty()) {
      result += '-';
      result += storageTag;
    }
    result += '-';
    result += std::to_string(profileName.size());
    result += '-';
    result.append(profileName.data(), profileName.size());
    return result;
  }

  static uint64_t descriptorOffset(uint64_t recordGeneration,
                                   uint64_t copy) {
    return ((recordGeneration & 1) * 2 + copy) * kProfileLogV4RecordSize;
  }

  static uint64_t phaseOffset(uint64_t copy) {
    return (4 + copy) * kProfileLogV4RecordSize;
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

  int admitBootstrapDirectoryOperationLocked() const {
    return bootstrapComplete ? -ESHUTDOWN : 0;
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
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
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
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
    DirectOPFSOperation operation(*profileLeaseState, terminalCloseState);
    if (!operation) {
      return operation.getError();
    }
    int childType = 0;
    int childID = -EIO;
    if (!proxy([&](auto ctx) {
          _wasmfs_opfs_get_child(
            ctx.ctx, kOPFSRootDirectoryID, name.c_str(), &childType, &childID);
        })) {
      return recordInitialisationProxyFailure();
    }
    if (childID == -ENOENT) {
      return -ENOENT;
    }
    return childType == 1 && childID == 0 ? 0
                                          : childID < 0 ? childID : -EIO;
  }

  int insertPhysicalFileLocked(const std::string& name) {
    if (int error = admitBootstrapDirectoryOperationLocked()) {
      return error;
    }
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
    for (size_t i = 0; i != arenas.size(); ++i) {
      if (int error = openFileLocked(arenas[i], &arenasOpen[i])) {
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
    int firstError = 0;
    auto closeOne = [&](const std::shared_ptr<OPFSFile>& file, bool* opened) {
      int error = closeFileForRetirementLocked(file, opened, flushFirst);
      if (!firstError && error) {
        firstError = error;
      }
      return terminalCloseState->getUnacknowledgedProxyError() == 0;
    };
    if (!closeOne(arenas[0], &arenasOpen[0]) ||
        !closeOne(arenas[1], &arenasOpen[1]) ||
        !closeOne(control, &controlOpen) ||
        !closeOne(bootstrap, &bootstrapOpen)) {
      return firstError ? firstError
                        : terminalCloseState->getUnacknowledgedProxyError();
    }
    return firstError;
  }

  void abandonPhysicalFilesLocked() {
    if (bootstrap) {
      bootstrap->abandonForTerminalFailure();
    }
    if (control) {
      control->abandonForTerminalFailure();
    }
    for (const auto& arena : arenas) {
      if (arena) {
        arena->abandonForTerminalFailure();
      }
    }
  }

  int stopAfterUnacknowledgedProxyLocked() {
    const int error = terminalCloseState->getUnacknowledgedProxyError();
    if (!error) {
      return 0;
    }
    abandonPhysicalFilesLocked();
    profileLeaseState->closeDestructorProxying();
    return error;
  }

  int setFixedSizeLocked(const std::shared_ptr<OPFSFile>& file, size_t size) {
    if (!file || size > static_cast<size_t>(std::numeric_limits<off_t>::max())) {
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

  int writeBytesAndFlushLocked(const std::shared_ptr<OPFSFile>& file,
                               uint64_t offset,
                               const uint8_t* data,
                               size_t size) {
    if (!file || (size && !data) ||
        offset > kProfileLogV4MaxSafeOffset ||
        offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        size > kProfileLogV4MaxSafeOffset - offset ||
        size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) -
                 offset) {
      return -EFBIG;
    }
    for (size_t cursor = 0; cursor != size;) {
      const size_t count = std::min(kProfileLogV4TransferSize, size - cursor);
      const ssize_t written = file->locked().write(
        data + cursor, count, static_cast<off_t>(offset + cursor));
      if (written < 0) {
        return written;
      }
      if (size_t(written) != count) {
        return -EIO;
      }
      cursor += count;
    }
    return file->locked().flush();
  }

  int readBytesLocked(const std::shared_ptr<OPFSFile>& file,
                      uint64_t offset,
                      uint8_t* data,
                      size_t size) {
    if (!file || (size && !data) ||
        offset > kProfileLogV4MaxSafeOffset ||
        offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        size > kProfileLogV4MaxSafeOffset - offset ||
        size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) -
                 offset) {
      return -EFBIG;
    }
    for (size_t cursor = 0; cursor != size;) {
      const size_t count = std::min(kProfileLogV4TransferSize, size - cursor);
      const ssize_t read = file->locked().read(
        data + cursor, count, static_cast<off_t>(offset + cursor));
      if (read < 0) {
        return read;
      }
      if (size_t(read) != count) {
        return -EIO;
      }
      cursor += count;
    }
    return 0;
  }

  int streamChecksumLocked(const std::shared_ptr<OPFSFile>& file,
                           uint64_t offset,
                           uint64_t size,
                           uint64_t* result) {
    if (!result || size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        offset > kProfileLogV4MaxSafeOffset ||
        size > kProfileLogV4MaxSafeOffset - offset) {
      return -EFBIG;
    }
    std::array<uint8_t, kProfileLogV4TransferSize> transfer;
    uint64_t checksum = UINT64_C(1469598103934665603);
    uint64_t cursor = 0;
    while (cursor != size) {
      const size_t count = static_cast<size_t>(
        std::min<uint64_t>(transfer.size(), size - cursor));
      if (int error = readBytesLocked(file, offset + cursor, transfer.data(), count)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION == 4
      if (cursor == 0 && count) {
        transfer[0] ^= 1;
      }
#endif
      checksum = profileLogV4ChecksumUpdate(checksum, transfer.data(), count);
      cursor += count;
    }
    *result = checksum;
    return 0;
  }

  int writeRecordLocked(
    const std::shared_ptr<OPFSFile>& file,
    uint64_t offset,
    const std::array<uint8_t, kProfileLogV4RecordSize>& record) {
    return writeBytesAndFlushLocked(file, offset, record.data(), record.size());
  }

  int readRecordLocked(
    const std::shared_ptr<OPFSFile>& file,
    uint64_t offset,
    std::array<uint8_t, kProfileLogV4RecordSize>* record) {
    if (!record) {
      return -EINVAL;
    }
    return readBytesLocked(file, offset, record->data(), record->size());
  }

  std::array<uint8_t, kProfileLogV4RecordSize> makeBootstrapRecord(
    ProfileLogV4BootstrapState state = ProfileLogV4BootstrapState::Ready,
    uint64_t bootstrapGeneration = 0) const {
    std::array<uint8_t, kProfileLogV4RecordSize> record = {};
    std::copy(kProfileLogV4BootstrapMagic.begin(),
              kProfileLogV4BootstrapMagic.end(),
              record.begin());
    writeProfileLogV4U32(record, 8, kProfileLogV4FormatVersion);
    writeProfileLogV4U32(record, 12, kProfileLogV4RecordSize);
    writeProfileLogV4U64(record, 16, profileChecksum);
    writeProfileLogV4U32(record, 24, profileLength);
    writeProfileLogV4U32(record, 28, arenas.size());
    writeProfileLogV4U32(record, 32, static_cast<uint32_t>(state));
    writeProfileLogV4U32(record, 36, kProfileLogV4LayoutEpoch);
    writeProfileLogV4U64(record, 48, bootstrapGeneration);
    writeProfileLogV4U64(
      record, 40, profileLogV4ChecksumWithZeroedRange(record, 40, 8));
    return record;
  }

  std::optional<ProfileLogV4BootstrapRecord> parseBootstrapRecord(
    const std::array<uint8_t, kProfileLogV4RecordSize>& record) const {
    if (!std::equal(kProfileLogV4BootstrapMagic.begin(),
                    kProfileLogV4BootstrapMagic.end(),
                    record.begin()) ||
        readProfileLogV4U32(record, 8) != kProfileLogV4FormatVersion ||
        readProfileLogV4U32(record, 12) != kProfileLogV4RecordSize ||
        readProfileLogV4U64(record, 16) != profileChecksum ||
        readProfileLogV4U32(record, 24) != profileLength ||
        readProfileLogV4U32(record, 28) != arenas.size() ||
        readProfileLogV4U32(record, 36) != kProfileLogV4LayoutEpoch ||
        readProfileLogV4U64(record, 40) !=
          profileLogV4ChecksumWithZeroedRange(record, 40, 8)) {
      return std::nullopt;
    }
    const uint32_t rawState = readProfileLogV4U32(record, 32);
    const uint64_t bootstrapGeneration = readProfileLogV4U64(record, 48);
    if (!bootstrapGeneration) {
      // The original V4 opaque-manifest experiment has a READY-only pair
      // with a zero tail. Preserve it as an established legacy layout.
      if (rawState != kProfileLogV4BootstrapReady ||
          !profileLogV4HasZeroTail(record, 48)) {
        return std::nullopt;
      }
      return ProfileLogV4BootstrapRecord{
        ProfileLogV4BootstrapState::Ready, 0, true};
    }
    if (!profileLogV4HasZeroTail(record, 56)) {
      return std::nullopt;
    }
    if (rawState == kProfileLogV4BootstrapPrepared &&
        bootstrapGeneration == kProfileLogV4BootstrapPreparedGeneration) {
      return ProfileLogV4BootstrapRecord{
        ProfileLogV4BootstrapState::Prepared, bootstrapGeneration, false};
    }
    if (rawState == kProfileLogV4BootstrapReady &&
        (bootstrapGeneration == kProfileLogV4BootstrapReadyFirstGeneration ||
         bootstrapGeneration ==
           kProfileLogV4BootstrapReadySecondGeneration)) {
      return ProfileLogV4BootstrapRecord{
        ProfileLogV4BootstrapState::Ready, bootstrapGeneration, false};
    }
    return std::nullopt;
  }

  ProfileLogV4BootstrapDisposition classifyBootstrapLocked(
    bool liveValidation = false) {
    std::array<uint8_t, kProfileLogV4RecordSize> first;
    std::array<uint8_t, kProfileLogV4RecordSize> second;
    if (readRecordLocked(bootstrap, 0, &first)) {
      return ProfileLogV4BootstrapDisposition::Invalid;
    }
    if (readRecordLocked(bootstrap, kProfileLogV4RecordSize, &second)) {
      return ProfileLogV4BootstrapDisposition::Invalid;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION == 2
    if (liveValidation) {
      first[0] ^= 1;
    }
#else
    (void)liveValidation;
#endif
    const auto firstRecord = parseBootstrapRecord(first);
    const auto secondRecord = parseBootstrapRecord(second);
    const bool firstEmpty = std::all_of(
      first.begin(), first.end(), [](uint8_t value) { return value == 0; });
    const bool secondEmpty = std::all_of(
      second.begin(), second.end(), [](uint8_t value) { return value == 0; });
    if (firstEmpty && secondEmpty) {
      return ProfileLogV4BootstrapDisposition::Empty;
    }
    if (firstRecord && secondRecord && firstRecord->legacy &&
        secondRecord->legacy && first == second) {
      return ProfileLogV4BootstrapDisposition::Established;
    }
    if (firstRecord && secondRecord && !firstRecord->legacy &&
        !secondRecord->legacy &&
        firstRecord->state == ProfileLogV4BootstrapState::Prepared &&
        firstRecord->generation ==
          kProfileLogV4BootstrapPreparedGeneration &&
        secondRecord->state == ProfileLogV4BootstrapState::Prepared &&
        secondRecord->generation ==
          kProfileLogV4BootstrapPreparedGeneration) {
      return ProfileLogV4BootstrapDisposition::Prepared;
    }
    if (firstRecord && secondRecord && !firstRecord->legacy &&
        !secondRecord->legacy &&
        firstRecord->state == ProfileLogV4BootstrapState::Ready &&
        firstRecord->generation ==
          kProfileLogV4BootstrapReadyFirstGeneration &&
        secondRecord->state == ProfileLogV4BootstrapState::Prepared &&
        secondRecord->generation ==
          kProfileLogV4BootstrapPreparedGeneration) {
      return ProfileLogV4BootstrapDisposition::InitialGeneration;
    }
    if (firstRecord && secondRecord && !firstRecord->legacy &&
        !secondRecord->legacy &&
        firstRecord->state == ProfileLogV4BootstrapState::Ready &&
        firstRecord->generation ==
          kProfileLogV4BootstrapReadyFirstGeneration &&
        secondRecord->state == ProfileLogV4BootstrapState::Ready &&
        secondRecord->generation ==
          kProfileLogV4BootstrapReadySecondGeneration) {
      return ProfileLogV4BootstrapDisposition::Established;
    }
    return ProfileLogV4BootstrapDisposition::Invalid;
  }

  std::array<uint8_t, kProfileLogV4RecordSize> makeDescriptorRecord(
    const ProfileLogV4Descriptor& descriptor) const {
    std::array<uint8_t, kProfileLogV4RecordSize> record = {};
    std::copy(kProfileLogV4DescriptorMagic.begin(),
              kProfileLogV4DescriptorMagic.end(),
              record.begin());
    writeProfileLogV4U32(record, 8, kProfileLogV4FormatVersion);
    writeProfileLogV4U32(record, 12, kProfileLogV4RecordSize);
    writeProfileLogV4U64(record, 16, descriptor.generation);
    writeProfileLogV4U32(record, 24, descriptor.arena);
    writeProfileLogV4U64(record, 32, descriptor.manifestOffset);
    writeProfileLogV4U64(record, 40, descriptor.manifestSize);
    writeProfileLogV4U64(record, 48, descriptor.manifestRecordChecksum);
    writeProfileLogV4U64(record, 56, descriptor.highWater[0]);
    writeProfileLogV4U64(record, 64, descriptor.highWater[1]);
    writeProfileLogV4U64(record, 72, profileChecksum);
    writeProfileLogV4U64(record, 80, kProfileLogV4LayoutEpoch);
    writeProfileLogV4U64(
      record, 88, profileLogV4ChecksumWithZeroedRange(record, 88, 8));
    return record;
  }

  std::optional<ProfileLogV4Descriptor> parseDescriptorRecord(
    const std::array<uint8_t, kProfileLogV4RecordSize>& record) const {
    if (!std::equal(kProfileLogV4DescriptorMagic.begin(),
                    kProfileLogV4DescriptorMagic.end(),
                    record.begin()) ||
        readProfileLogV4U32(record, 8) != kProfileLogV4FormatVersion ||
        readProfileLogV4U32(record, 12) != kProfileLogV4RecordSize ||
        !profileLogV4HasZeroRange(record, 28, 4) ||
        readProfileLogV4U64(record, 72) != profileChecksum ||
        readProfileLogV4U64(record, 80) != kProfileLogV4LayoutEpoch ||
        readProfileLogV4U64(record, 88) !=
          profileLogV4ChecksumWithZeroedRange(record, 88, 8) ||
        !profileLogV4HasZeroTail(record, 96)) {
      return std::nullopt;
    }
    ProfileLogV4Descriptor descriptor = {
      readProfileLogV4U64(record, 16),
      readProfileLogV4U32(record, 24),
      readProfileLogV4U64(record, 32),
      readProfileLogV4U64(record, 40),
      readProfileLogV4U64(record, 48),
      {readProfileLogV4U64(record, 56), readProfileLogV4U64(record, 64)},
      readProfileLogV4U64(record, 88),
    };
    if (!descriptor.generation || descriptor.arena >= arenas.size() ||
        descriptor.manifestOffset > kProfileLogV4MaxSafeOffset ||
        descriptor.manifestSize > kProfileLogV4MaxSafeOffset ||
        descriptor.highWater[0] > kProfileLogV4MaxSafeOffset ||
        descriptor.highWater[1] > kProfileLogV4MaxSafeOffset) {
      return std::nullopt;
    }
    return descriptor;
  }

  std::array<uint8_t, kProfileLogV4RecordSize> makePhaseRecord(
    uint64_t recordGeneration,
    uint32_t arena,
    uint64_t descriptorChecksum) const {
    std::array<uint8_t, kProfileLogV4RecordSize> record = {};
    std::copy(kProfileLogV4PhaseMagic.begin(),
              kProfileLogV4PhaseMagic.end(),
              record.begin());
    writeProfileLogV4U32(record, 8, kProfileLogV4FormatVersion);
    writeProfileLogV4U32(record, 12, kProfileLogV4RecordSize);
    writeProfileLogV4U64(record, 16, recordGeneration);
    writeProfileLogV4U32(record, 24, arena);
    writeProfileLogV4U32(record, 28, kProfileLogV4PhaseClean);
    writeProfileLogV4U64(record, 32, descriptorChecksum);
    writeProfileLogV4U64(record, 40, profileChecksum);
    writeProfileLogV4U64(
      record, 48, profileLogV4ChecksumWithZeroedRange(record, 48, 8));
    return record;
  }

  std::optional<ProfileLogV4Phase> parsePhaseRecord(
    const std::array<uint8_t, kProfileLogV4RecordSize>& record) const {
    if (!std::equal(kProfileLogV4PhaseMagic.begin(),
                    kProfileLogV4PhaseMagic.end(),
                    record.begin()) ||
        readProfileLogV4U32(record, 8) != kProfileLogV4FormatVersion ||
        readProfileLogV4U32(record, 12) != kProfileLogV4RecordSize ||
        readProfileLogV4U32(record, 28) != kProfileLogV4PhaseClean ||
        readProfileLogV4U64(record, 40) != profileChecksum ||
        readProfileLogV4U64(record, 48) !=
          profileLogV4ChecksumWithZeroedRange(record, 48, 8) ||
        !profileLogV4HasZeroTail(record, 56)) {
      return std::nullopt;
    }
    const uint64_t recordGeneration = readProfileLogV4U64(record, 16);
    const uint32_t arena = readProfileLogV4U32(record, 24);
    if (!recordGeneration || arena >= arenas.size()) {
      return std::nullopt;
    }
    return ProfileLogV4Phase{recordGeneration,
                             arena,
                             readProfileLogV4U64(record, 32)};
  }

  std::array<uint8_t, kProfileLogV4ManifestHeaderSize> makeManifestHeader(
    uint64_t recordGeneration,
    const uint8_t* data,
    size_t size) const {
    std::array<uint8_t, kProfileLogV4ManifestHeaderSize> header = {};
    std::copy(kProfileLogV4ManifestMagic.begin(),
              kProfileLogV4ManifestMagic.end(),
              header.begin());
    writeProfileLogV4U32(header, 8, kProfileLogV4FormatVersion);
    writeProfileLogV4U32(header, 12, kProfileLogV4ManifestHeaderSize);
    writeProfileLogV4U64(header, 16, recordGeneration);
    writeProfileLogV4U64(header, 24, size);
    writeProfileLogV4U64(header, 32, profileLogV4Checksum(data, size));
    writeProfileLogV4U64(header, 40, profileChecksum);
    writeProfileLogV4U64(header, 48, kProfileLogV4LayoutEpoch);
    writeProfileLogV4U64(
      header, 56, profileLogV4ChecksumWithZeroedRange(header, 56, 8));
    return header;
  }

  std::optional<ProfileLogV4ManifestHeader> parseManifestHeader(
    const std::array<uint8_t, kProfileLogV4ManifestHeaderSize>& header) const {
    if (!std::equal(kProfileLogV4ManifestMagic.begin(),
                    kProfileLogV4ManifestMagic.end(),
                    header.begin()) ||
        readProfileLogV4U32(header, 8) != kProfileLogV4FormatVersion ||
        readProfileLogV4U32(header, 12) != kProfileLogV4ManifestHeaderSize ||
        readProfileLogV4U64(header, 40) != profileChecksum ||
        readProfileLogV4U64(header, 48) != kProfileLogV4LayoutEpoch ||
        readProfileLogV4U64(header, 56) !=
          profileLogV4ChecksumWithZeroedRange(header, 56, 8) ||
        !profileLogV4HasZeroTail(header, 64)) {
      return std::nullopt;
    }
    const uint64_t recordGeneration = readProfileLogV4U64(header, 16);
    const uint64_t size = readProfileLogV4U64(header, 24);
    if (!recordGeneration || size > kProfileLogV4MaxSafeOffset -
                                      kProfileLogV4ManifestHeaderSize) {
      return std::nullopt;
    }
    return ProfileLogV4ManifestHeader{recordGeneration,
                                      size,
                                      readProfileLogV4U64(header, 32),
                                      readProfileLogV4U64(header, 56)};
  }

  int writeDescriptorPairLocked(const ProfileLogV4Descriptor& descriptor,
                                uint64_t* descriptorChecksum) {
    const auto record = makeDescriptorRecord(descriptor);
    if (descriptorChecksum) {
      *descriptorChecksum = readProfileLogV4U64(record, 88);
    }
    for (uint64_t copy = 0; copy != 2; ++copy) {
      if (int error = writeRecordLocked(
            control, descriptorOffset(descriptor.generation, copy), record)) {
        return error;
      }
    }
    return 0;
  }

  int writePhaseWitnessLocked(uint64_t copy,
                              uint64_t recordGeneration,
                              uint32_t arena,
                              uint64_t descriptorChecksum) {
    return writeRecordLocked(control,
                             phaseOffset(copy),
                             makePhaseRecord(recordGeneration,
                                             arena,
                                             descriptorChecksum));
  }

  int writeGenerationLocked(uint64_t recordGeneration,
                            const uint8_t* data,
                            size_t size,
                            const std::array<uint64_t, 2>& startHighWater,
                            ProfileLogV4Descriptor* result) {
    const uint64_t byteSize = size;
    if (!recordGeneration || (size && !data) ||
        byteSize >
          kProfileLogV4MaxSafeOffset - kProfileLogV4ManifestHeaderSize) {
      return -EINVAL;
    }
    const uint32_t arena = recordGeneration & 1;
    const uint64_t manifestOffset = startHighWater[arena];
    const uint64_t payloadOffset =
      manifestOffset + kProfileLogV4ManifestHeaderSize;
    const uint64_t end = payloadOffset + byteSize;
    if (manifestOffset > kProfileLogV4MaxSafeOffset -
                           kProfileLogV4ManifestHeaderSize ||
        payloadOffset > kProfileLogV4MaxSafeOffset - byteSize ||
        end > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return -EFBIG;
    }
    const auto header = makeManifestHeader(recordGeneration, data, size);
    if (size) {
      if (int error = writeBytesAndFlushLocked(
            arenas[arena], payloadOffset, data, size)) {
        return error;
      }
    }
    if (int error = writeBytesAndFlushLocked(
          arenas[arena], manifestOffset, header.data(), header.size())) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE == 1
    if (consumeProfileLogV4ProxyCompletionForTesting()) {
      // Controlled acknowledgement loss at the common V4 publication
      // boundary. The complete next manifest is durable, including a
      // namespace-only rename transaction, but no descriptor or phase witness
      // can make it selected. This is not a literal ProxyWorker failure or a
      // crash simulation.
      ++profileLogV4ProxyCompletionLatchCountForTesting;
      latchProfileLogV4ProxyCompletionForTesting();
      controlledProxyCompletionFailureForTesting = true;
      terminalCloseState
        ->recordControlledUnacknowledgedProxyCompletionForTesting();
      return poisonLocked(-EIO);
    }
#endif
    auto nextHighWater = startHighWater;
    nextHighWater[arena] = end;
    ProfileLogV4Descriptor descriptor = {recordGeneration,
                                         arena,
                                         manifestOffset,
                                         byteSize,
                                         readProfileLogV4U64(header, 56),
                                         nextHighWater,
                                         0};
    uint64_t descriptorChecksum = 0;
    if (int error = writeDescriptorPairLocked(descriptor, &descriptorChecksum)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
    // Both descriptor copies are durable here, but the previous equal phase
    // pair remains the only selector authorization until a witness changes.
    // Keep this test-only interruption boundary ahead of the first witness so
    // recovery proves that the appended manifest is still unreachable.
    wasmfs_opfs_profile_log_v4_test_maybe_interrupt(10);
#endif
    if (int error = writePhaseWitnessLocked(
          0, recordGeneration, arena, descriptorChecksum)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v4_test_maybe_interrupt(1);
#endif
    if (int error = writePhaseWitnessLocked(
          1, recordGeneration, arena, descriptorChecksum)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v4_test_maybe_interrupt(2);
#endif
    descriptor.recordChecksum = descriptorChecksum;
    if (result) {
      *result = descriptor;
    }
    return 0;
  }

  int writeGenerationLocked(uint64_t recordGeneration,
                            const uint8_t* data,
                            size_t size,
                            ProfileLogV4Descriptor* result) {
    return writeGenerationLocked(
      recordGeneration, data, size, highWater, result);
  }

  int validatePhaseManifestLocked(const ProfileLogV4Phase& phase,
                                  ProfileLogV4Descriptor* result,
                                  bool validatePayload = true) {
    if (!result || phase.arena != (phase.generation & 1)) {
      return -EIO;
    }
    std::array<std::optional<ProfileLogV4Descriptor>, 2> descriptors;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV4RecordSize> record;
      if (int error = readRecordLocked(
            control, descriptorOffset(phase.generation, copy), &record)) {
        return error;
      }
      descriptors[copy] = parseDescriptorRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION == 2
    descriptors[0].reset();
#endif
    if (!descriptors[0] || !descriptors[1] ||
        descriptors[0]->generation != phase.generation ||
        descriptors[1]->generation != phase.generation ||
        descriptors[0]->arena != phase.arena ||
        descriptors[1]->arena != phase.arena ||
        descriptors[0]->manifestOffset != descriptors[1]->manifestOffset ||
        descriptors[0]->manifestSize != descriptors[1]->manifestSize ||
        descriptors[0]->manifestRecordChecksum !=
          descriptors[1]->manifestRecordChecksum ||
        descriptors[0]->highWater != descriptors[1]->highWater ||
        descriptors[0]->recordChecksum != phase.descriptorChecksum ||
        descriptors[1]->recordChecksum != phase.descriptorChecksum) {
      return -EIO;
    }
    const ProfileLogV4Descriptor& descriptor = *descriptors[0];
    if (descriptor.manifestOffset > kProfileLogV4MaxSafeOffset -
                                      kProfileLogV4ManifestHeaderSize ||
        descriptor.manifestSize > kProfileLogV4MaxSafeOffset -
                                      descriptor.manifestOffset -
                                      kProfileLogV4ManifestHeaderSize ||
        descriptor.highWater[descriptor.arena] <
          descriptor.manifestOffset + kProfileLogV4ManifestHeaderSize +
            descriptor.manifestSize) {
      return -EIO;
    }
    for (size_t i = 0; i != arenas.size(); ++i) {
      const off_t physicalSize = arenas[i]->locked().getSize();
      if (physicalSize < 0) {
        return physicalSize;
      }
      if (static_cast<uint64_t>(physicalSize) < descriptor.highWater[i]) {
        return -EIO;
      }
    }
    std::array<uint8_t, kProfileLogV4ManifestHeaderSize> header;
    if (int error = readBytesLocked(arenas[descriptor.arena],
                                    descriptor.manifestOffset,
                                    header.data(),
                                    header.size())) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION == 3
    header[0] ^= 1;
#endif
    auto parsed = parseManifestHeader(header);
    if (!parsed || parsed->generation != phase.generation ||
        parsed->size != descriptor.manifestSize ||
        parsed->recordChecksum != descriptor.manifestRecordChecksum) {
      return -EIO;
    }
    if (validatePayload) {
      uint64_t payloadChecksum = 0;
      if (int error = streamChecksumLocked(
            arenas[descriptor.arena],
            descriptor.manifestOffset + kProfileLogV4ManifestHeaderSize,
            descriptor.manifestSize,
            &payloadChecksum)) {
        return error;
      }
      if (payloadChecksum != parsed->payloadChecksum) {
        return -EIO;
      }
    }
    *result = descriptor;
    return 0;
  }

  int recoverControlLocked() {
    std::array<std::optional<ProfileLogV4Phase>, 2> phases;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV4RecordSize> record;
      if (int error = readRecordLocked(control, phaseOffset(copy), &record)) {
        return error;
      }
      phases[copy] = parsePhaseRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION == 1
    phases[0].reset();
#endif
    if (!phases[0] || !phases[1]) {
      return -EIO;
    }
    ProfileLogV4Descriptor selected;
    if (phases[0]->generation == phases[1]->generation) {
      if (phases[0]->arena != phases[1]->arena ||
          phases[0]->descriptorChecksum != phases[1]->descriptorChecksum) {
        return -EIO;
      }
      if (int error = validatePhaseManifestLocked(*phases[0], &selected)) {
        return error;
      }
    } else {
      const uint64_t oldCopy = phases[0]->generation < phases[1]->generation
                                 ? 0
                                 : 1;
      const uint64_t newCopy = oldCopy ^ 1;
      const ProfileLogV4Phase& oldPhase = *phases[oldCopy];
      const ProfileLogV4Phase& newPhase = *phases[newCopy];
      if (oldPhase.generation == std::numeric_limits<uint64_t>::max() ||
          newPhase.generation != oldPhase.generation + 1 ||
          oldPhase.arena != (oldPhase.generation & 1) ||
          newPhase.arena != (newPhase.generation & 1)) {
        return -EIO;
      }
      if (int error = validatePhaseManifestLocked(oldPhase, &selected)) {
        return error;
      }
      // A one-witness new generation is not exposed. Deliberately retain the
      // g/g+1 split: rewriting the newer witness in place could itself tear
      // and destroy the only durable old witness. The next commit reuses the
      // unselected g+1 descriptor slots and publishes them normally.
    }
    generation = selected.generation;
    highWater = selected.highWater;
    selectedDescriptor = selected;
    hasSelectedDescriptor = true;
    return 0;
  }

  int verifyBootstrapLocked(bool liveValidation = false) {
    return classifyBootstrapLocked(liveValidation) ==
             ProfileLogV4BootstrapDisposition::Established
             ? 0
             : -EIO;
  }

  void resetInitialBootstrapFileStateLocked() {
    bootstrap.reset();
    control.reset();
    arenas = {};
    bootstrapOpen = false;
    controlOpen = false;
    arenasOpen = {};
    generation = 0;
    highWater = {};
    selectedDescriptor = {};
    hasSelectedDescriptor = false;
  }

  int closeInitialBootstrapInspectionLocked() {
    if (int error = closeAllFilesForRetirementLocked(false)) {
      return error;
    }
    resetInitialBootstrapFileStateLocked();
    return 0;
  }

  int discardPreparedInitialBootstrapLocked() {
    // Both PREPARED witnesses were made durable before any sibling could be
    // created and before a caller could receive this backend. Keep that pair
    // until the sibling names have gone, then delete bootstrap last. If the
    // document stops during cleanup, the next fresh factory sees the same
    // PREPARED authority and repeats this idempotent sequence.
    if (int error = closeInitialBootstrapInspectionLocked()) {
      return error;
    }
    const std::array<std::string, 4> names = {
      arenaNames[0], arenaNames[1], controlName, bootstrapName};
    for (size_t index = 0; index != names.size(); ++index) {
      const std::string& name = names[index];
      const int status = lookupPhysicalFileLocked(name);
      if (status == -ENOENT) {
        continue;
      }
      if (status) {
        return status;
      }
      if (int error = removeRecoverableBootstrapPhysicalFileLocked(name)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
      wasmfs_opfs_profile_log_v4_test_maybe_interrupt(
        5 + static_cast<int>(index));
#endif
    }
    return 0;
  }

  int openInitialGenerationLocked() {
    // R1/P1 is recoverable only when the envelope proves that it is exactly
    // the unexposed, empty g=1 filesystem state. A damaged established
    // profile may never be reclassified as resettable just because one
    // bootstrap record happens to parse.
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    arenas[0] = makePhysicalFile(arenaNames[0]);
    arenas[1] = makePhysicalFile(arenaNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = requireFixedSizeLocked(
          bootstrap, kProfileLogV4BootstrapSize)) {
      return error;
    }
    if (int error = requireFixedSizeLocked(control, kProfileLogV4ControlSize)) {
      return error;
    }
    if (int error = recoverControlLocked()) {
      return error;
    }
    const std::array<uint64_t, 2> initialHighWater = {
      0, kProfileLogV4ManifestHeaderSize};
    if (generation != 1 || !hasSelectedDescriptor ||
        selectedDescriptor.generation != 1 || selectedDescriptor.arena != 1 ||
        selectedDescriptor.manifestOffset != 0 ||
        selectedDescriptor.manifestSize != 0 ||
        selectedDescriptor.highWater != initialHighWater) {
      return -EIO;
    }
    for (size_t index = 0; index != arenas.size(); ++index) {
      const off_t size = arenas[index]->locked().getSize();
      if (size < 0) {
        return size;
      }
      if (static_cast<uint64_t>(size) != initialHighWater[index]) {
        return -EIO;
      }
    }
    return 0;
  }

  int completeInitialBootstrapLocked() {
    const auto secondReady = makeBootstrapRecord(
      ProfileLogV4BootstrapState::Ready,
      kProfileLogV4BootstrapReadySecondGeneration);
    if (int error = writeRecordLocked(
          bootstrap, kProfileLogV4RecordSize, secondReady)) {
      return error;
    }
    return verifyBootstrapLocked();
  }

  int initialiseFreshLocked() {
    if (recoverInitialBootstrap) {
      // The filesystem is the only V4 client with an explicit first-creation
      // recovery protocol. Its mirrored PREPARED pair is durable before any
      // sibling name exists and before this factory can return a mountable
      // root. The opaque V4 manifest primitive keeps the legacy READY-only
      // sequence below and remains deliberately fail-closed on interruption.
      if (int error = insertPhysicalFileLocked(bootstrapName)) {
        return error;
      }
      bootstrap = makePhysicalFile(bootstrapName);
      if (int error = openFileLocked(bootstrap, &bootstrapOpen)) {
        return error;
      }
      if (int error = setFixedSizeLocked(
            bootstrap, kProfileLogV4BootstrapSize)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
      wasmfs_opfs_profile_log_v4_test_maybe_interrupt(-2);
#endif
      const auto prepared = makeBootstrapRecord(
        ProfileLogV4BootstrapState::Prepared,
        kProfileLogV4BootstrapPreparedGeneration);
      if (int error = writeRecordLocked(bootstrap, 0, prepared)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
      wasmfs_opfs_profile_log_v4_test_maybe_interrupt(-1);
#endif
      if (int error = writeRecordLocked(
            bootstrap, kProfileLogV4RecordSize, prepared)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
      wasmfs_opfs_profile_log_v4_test_maybe_interrupt(0);
#endif

      for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
        if (int error = insertPhysicalFileLocked(name)) {
          return error;
        }
      }
      control = makePhysicalFile(controlName);
      arenas[0] = makePhysicalFile(arenaNames[0]);
      arenas[1] = makePhysicalFile(arenaNames[1]);
      if (int error = openFileLocked(control, &controlOpen)) {
        return error;
      }
      if (int error = openFileLocked(arenas[0], &arenasOpen[0])) {
        return error;
      }
      if (int error = openFileLocked(arenas[1], &arenasOpen[1])) {
        return error;
      }
      if (int error = setFixedSizeLocked(control, kProfileLogV4ControlSize)) {
        return error;
      }
      ProfileLogV4Descriptor initial;
      if (int error = writeGenerationLocked(1, nullptr, 0, &initial)) {
        return error;
      }
      const auto firstReady = makeBootstrapRecord(
        ProfileLogV4BootstrapState::Ready,
        kProfileLogV4BootstrapReadyFirstGeneration);
      if (int error = writeRecordLocked(bootstrap, 0, firstReady)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
      wasmfs_opfs_profile_log_v4_test_maybe_interrupt(3);
#endif
      const auto secondReady = makeBootstrapRecord(
        ProfileLogV4BootstrapState::Ready,
        kProfileLogV4BootstrapReadySecondGeneration);
      if (int error = writeRecordLocked(
            bootstrap, kProfileLogV4RecordSize, secondReady)) {
        return error;
      }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
      wasmfs_opfs_profile_log_v4_test_maybe_interrupt(4);
#endif
      generation = initial.generation;
      highWater = initial.highWater;
      selectedDescriptor = initial;
      hasSelectedDescriptor = true;
      return 0;
    }

    for (const auto& name : {arenaNames[0], arenaNames[1], controlName,
                             bootstrapName}) {
      if (int error = insertPhysicalFileLocked(name)) {
        return error;
      }
    }
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    arenas[0] = makePhysicalFile(arenaNames[0]);
    arenas[1] = makePhysicalFile(arenaNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = setFixedSizeLocked(bootstrap, kProfileLogV4BootstrapSize)) {
      return error;
    }
    if (int error = setFixedSizeLocked(control, kProfileLogV4ControlSize)) {
      return error;
    }
    ProfileLogV4Descriptor initial;
    if (int error = writeGenerationLocked(1, nullptr, 0, &initial)) {
      return error;
    }
    const auto bootstrapRecord = makeBootstrapRecord();
    if (int error = writeRecordLocked(bootstrap, 0, bootstrapRecord)) {
      return error;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v4_test_maybe_interrupt(0);
#endif
    if (int error = writeRecordLocked(
          bootstrap, kProfileLogV4RecordSize, bootstrapRecord)) {
      return error;
    }
    generation = initial.generation;
    highWater = initial.highWater;
    selectedDescriptor = initial;
    hasSelectedDescriptor = true;
    return 0;
  }

  int openEstablishedLocked() {
    bootstrap = makePhysicalFile(bootstrapName);
    control = makePhysicalFile(controlName);
    arenas[0] = makePhysicalFile(arenaNames[0]);
    arenas[1] = makePhysicalFile(arenaNames[1]);
    if (int error = openAllFilesLocked()) {
      return error;
    }
    if (int error = requireFixedSizeLocked(
          bootstrap, kProfileLogV4BootstrapSize)) {
      return error;
    }
    if (int error = requireFixedSizeLocked(control, kProfileLogV4ControlSize)) {
      return error;
    }
    if (int error = verifyBootstrapLocked()) {
      return error;
    }
    return recoverControlLocked();
  }

  int operationErrorLocked() const {
    return fatalError ? fatalError : hasSelectedDescriptor ? 0 : -ESHUTDOWN;
  }

  int validateSelectedManifestLocked(bool validatePayload = true) {
    if (!hasSelectedDescriptor) {
      return -ESHUTDOWN;
    }
    if (int error = requireFixedSizeLocked(
          bootstrap, kProfileLogV4BootstrapSize)) {
      return error;
    }
    if (int error = requireFixedSizeLocked(control, kProfileLogV4ControlSize)) {
      return error;
    }
    if (int error = verifyBootstrapLocked(true)) {
      return error;
    }
    std::array<std::optional<ProfileLogV4Phase>, 2> phases;
    for (uint64_t copy = 0; copy != 2; ++copy) {
      std::array<uint8_t, kProfileLogV4RecordSize> record;
      if (int error = readRecordLocked(control, phaseOffset(copy), &record)) {
        return error;
      }
      phases[copy] = parsePhaseRecord(record);
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION == 1
    phases[0].reset();
#endif
    if (!phases[0] || !phases[1]) {
      return -EIO;
    }
    const ProfileLogV4Phase* selectedPhase = nullptr;
    if (phases[0]->generation == phases[1]->generation) {
      if (phases[0]->arena != phases[1]->arena ||
          phases[0]->descriptorChecksum != phases[1]->descriptorChecksum) {
        return -EIO;
      }
      selectedPhase = &*phases[0];
    } else {
      const uint64_t oldCopy = phases[0]->generation < phases[1]->generation
                                 ? 0
                                 : 1;
      const uint64_t newCopy = oldCopy ^ 1;
      const ProfileLogV4Phase& oldPhase = *phases[oldCopy];
      const ProfileLogV4Phase& newPhase = *phases[newCopy];
      if (oldPhase.generation == std::numeric_limits<uint64_t>::max() ||
          newPhase.generation != oldPhase.generation + 1 ||
          oldPhase.arena != (oldPhase.generation & 1) ||
          newPhase.arena != (newPhase.generation & 1)) {
        return -EIO;
      }
      selectedPhase = &oldPhase;
    }
    if (selectedPhase->generation != selectedDescriptor.generation ||
        selectedPhase->arena != selectedDescriptor.arena ||
        selectedPhase->descriptorChecksum != selectedDescriptor.recordChecksum) {
      return -EIO;
    }
    ProfileLogV4Descriptor validated;
    if (int error = validatePhaseManifestLocked(
          *selectedPhase, &validated, validatePayload)) {
      return error;
    }
    return profileLogV4SameDescriptor(validated, selectedDescriptor) ? 0
                                                                      : -EIO;
  }

  // A transaction that reached an arena write but not the second phase
  // witness leaves an unreachable append tail. The selected descriptor is the
  // only reachability boundary: data below either high-water mark can still
  // be named by the current filesystem manifest, including data in the
  // inactive parity arena. Before the next logical transaction, reclaim only
  // bytes beyond those durable boundaries. If the physical file is shorter,
  // fail closed rather than treating a damaged selected extent as a tail.
  int trimUnreachableArenaTailsLocked() {
    if (!hasSelectedDescriptor) {
      return -ESHUTDOWN;
    }
    for (size_t index = 0; index != arenas.size(); ++index) {
      const uint64_t durableEnd = selectedDescriptor.highWater[index];
      if (!arenas[index]) {
        return -ESHUTDOWN;
      }
      if (durableEnd >
          static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return -EOVERFLOW;
      }
      const off_t physicalSize = arenas[index]->locked().getSize();
      if (physicalSize < 0) {
        return physicalSize;
      }
      const uint64_t physicalEnd = static_cast<uint64_t>(physicalSize);
      if (physicalEnd < durableEnd) {
        return -EIO;
      }
      if (physicalEnd == durableEnd) {
        continue;
      }
      if (int error = arenas[index]->locked().setSize(
            static_cast<off_t>(durableEnd))) {
        return error;
      }
      if (int error = arenas[index]->locked().flush()) {
        return error;
      }
      const off_t trimmedSize = arenas[index]->locked().getSize();
      if (trimmedSize < 0) {
        return trimmedSize;
      }
      if (static_cast<uint64_t>(trimmedSize) != durableEnd) {
        return -EIO;
      }
    }
    return 0;
  }

protected:
  explicit ProfileLogV4Store(std::string storageTag = {},
                             bool recoverInitialBootstrap = false)
    : storageTag(std::move(storageTag)),
      recoverInitialBootstrap(recoverInitialBootstrap) {}

  // Only a logical transaction builder may use this fast path. The caller
  // holds storeMutex through commitTransaction(), which has already validated
  // the selected descriptor and cannot publish a replacement until the
  // builder returns. Revalidating the whole outer manifest for each copied
  // checkpoint chunk would make one checkpoint quadratic in its live tree.
  int readSelectedBytesDuringValidatedTransaction(uint32_t arena,
                                                   uint64_t offset,
                                                   uint8_t* buffer,
                                                   size_t size) {
    if (!hasSelectedDescriptor || (!buffer && size) || arena >= arenas.size() ||
        offset > selectedDescriptor.highWater[arena] ||
        size > selectedDescriptor.highWater[arena] - offset) {
      return -EIO;
    }
    return readBytesLocked(arenas[arena], offset, buffer, size);
  }

  int trimSelectedUnreachableArenaTails() {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (int error = validateSelectedManifestLocked()) {
      return poisonLocked(error);
    }
    return trimUnreachableArenaTailsLocked();
  }

  int readManifest(uint8_t* buffer,
                   size_t capacity,
                   size_t* size,
                   uint64_t* manifestGeneration = nullptr) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (int error = validateSelectedManifestLocked()) {
      return poisonLocked(error);
    }
    if (!size || (!buffer && capacity)) {
      return -EINVAL;
    }
    if (selectedDescriptor.manifestSize >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return -EOVERFLOW;
    }
    const size_t required = selectedDescriptor.manifestSize;
    *size = required;
    if (manifestGeneration) {
      *manifestGeneration = selectedDescriptor.generation;
    }
    if (!buffer && !capacity) {
      return 0;
    }
    if (capacity < required) {
      return -ENOBUFS;
    }
    const int read = readBytesLocked(
      arenas[selectedDescriptor.arena],
      selectedDescriptor.manifestOffset + kProfileLogV4ManifestHeaderSize,
      buffer,
      required);
    return read ? poisonLocked(read) : 0;
  }

  // Read a byte range named by the currently selected descriptor. This
  // revalidates the V4 envelope on every call and never permits a logical
  // payload parser to read an arena tail that has not reached witness quorum.
  int readSelectedBytes(uint32_t arena,
                        uint64_t offset,
                        uint8_t* buffer,
                        size_t size) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (!buffer && size) {
      return -EINVAL;
    }
    if (int error = validateSelectedManifestLocked()) {
      return poisonLocked(error);
    }
    if (arena >= arenas.size() || offset > selectedDescriptor.highWater[arena] ||
        size > selectedDescriptor.highWater[arena] - offset) {
      return -EIO;
    }
    if (int error = readBytesLocked(arenas[arena], offset, buffer, size)) {
      return poisonLocked(error);
    }
    return 0;
  }

  int readSelectedBytesFromValidatedSnapshot(uint64_t expectedGeneration,
                                             uint32_t arena,
                                             uint64_t offset,
                                             uint8_t* buffer,
                                             size_t size) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (!buffer && size) {
      return -EINVAL;
    }
    if (expectedGeneration != selectedDescriptor.generation ||
        !expectedGeneration) {
      return poisonLocked(-EIO);
    }
    if (int error = validateSelectedManifestLocked(false)) {
      return poisonLocked(error);
    }
    if (arena >= arenas.size() || offset > selectedDescriptor.highWater[arena] ||
        size > selectedDescriptor.highWater[arena] - offset) {
      return poisonLocked(-EIO);
    }
    if (int error = readBytesLocked(arenas[arena], offset, buffer, size)) {
      return poisonLocked(error);
    }
    return 0;
  }

  // Read one historical manifest that remains reachable under the current
  // selected descriptor. A delta carries this locator verbatim; the selected
  // descriptor's high-water marks, the historical outer header, and the
  // historical payload checksum are all revalidated before it is returned.
  int readSelectedHistoricalManifest(
    const ProfileLogV4ManifestReference& reference,
    std::vector<uint8_t>* manifest) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (!manifest || !reference.generation ||
        reference.arena >= arenas.size() ||
        reference.arena != (reference.generation & 1) ||
        reference.generation >= selectedDescriptor.generation ||
        reference.manifestOffset >
          kProfileLogV4MaxSafeOffset - kProfileLogV4ManifestHeaderSize ||
        reference.manifestSize > kProfileLogV4MaxSafeOffset -
          reference.manifestOffset - kProfileLogV4ManifestHeaderSize) {
      return poisonLocked(-EIO);
    }
    if (int error = validateSelectedManifestLocked(false)) {
      return poisonLocked(error);
    }
    const uint64_t end = reference.manifestOffset +
                         kProfileLogV4ManifestHeaderSize +
                         reference.manifestSize;
    if (end > selectedDescriptor.highWater[reference.arena] ||
        reference.manifestSize >
          static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      return poisonLocked(-EIO);
    }
    std::array<uint8_t, kProfileLogV4ManifestHeaderSize> header;
    if (int error = readBytesLocked(arenas[reference.arena],
                                    reference.manifestOffset,
                                    header.data(),
                                    header.size())) {
      return poisonLocked(error);
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_PARENT_CORRUPTION
    // Schema-2 replay is the sole current caller. Fault only this stack-local
    // historical header after the selected envelope has authenticated; no
    // OPFS byte, descriptor, or phase witness is modified by this test hook.
    header[0] ^= 1;
#endif
    const auto parsed = parseManifestHeader(header);
    if (!parsed || parsed->generation != reference.generation ||
        parsed->size != reference.manifestSize ||
        parsed->recordChecksum != reference.manifestRecordChecksum) {
      return poisonLocked(-EIO);
    }
    uint64_t payloadChecksum = 0;
    if (int error = streamChecksumLocked(
          arenas[reference.arena],
          reference.manifestOffset + kProfileLogV4ManifestHeaderSize,
          reference.manifestSize,
          &payloadChecksum)) {
      return poisonLocked(error);
    }
    if (payloadChecksum != parsed->payloadChecksum) {
      return poisonLocked(-EIO);
    }
    manifest->resize(static_cast<size_t>(reference.manifestSize));
    if (int error = readBytesLocked(
          arenas[reference.arena],
          reference.manifestOffset + kProfileLogV4ManifestHeaderSize,
          manifest->data(), manifest->size())) {
      return poisonLocked(error);
    }
    return 0;
  }

  int commitManifest(const uint8_t* data, size_t size) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (size && !data) {
      return -EINVAL;
    }
    // Never replace a selected state that no longer validates. The opaque
    // caller might otherwise make a new generation appear to heal a corrupt
    // base without the future filesystem having had a chance to inspect it.
    if (int error = validateSelectedManifestLocked()) {
      return poisonLocked(error);
    }
    if (generation == std::numeric_limits<uint64_t>::max()) {
      return poisonLocked(-EOVERFLOW);
    }
    ProfileLogV4Descriptor next;
    if (int error = writeGenerationLocked(generation + 1, data, size, &next)) {
      return poisonLocked(error);
    }
    generation = next.generation;
    highWater = next.highWater;
    selectedDescriptor = next;
    hasSelectedDescriptor = true;
    return 0;
  }

  // Execute one logical payload transaction. The callback can append
  // immutable records to the selected arena and then must return the complete
  // manifest post-image. Only this function advances the V4 generation and
  // publishes its descriptor/witness quorum, so no manifest can reference an
  // unflushed or unselected record.
  int commitTransaction(const TransactionBuilder& builder) {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (int error = operationErrorLocked()) {
      return error;
    }
    if (!builder) {
      return -EINVAL;
    }
    if (int error = validateSelectedManifestLocked()) {
      return poisonLocked(error);
    }
    if (generation == std::numeric_limits<uint64_t>::max()) {
      return poisonLocked(-EOVERFLOW);
    }
    if (int error = trimUnreachableArenaTailsLocked()) {
      return poisonLocked(error);
    }
    Transaction transaction(*this,
                            generation + 1,
                            highWater,
                            profileLogV4ManifestReference(selectedDescriptor));
    std::vector<uint8_t> manifest;
    if (int error = builder(transaction, &manifest)) {
      return poisonLocked(error);
    }
    ProfileLogV4Descriptor next;
    if (int error = writeGenerationLocked(transaction.generation(),
                                          manifest.data(),
                                          manifest.size(),
                                          transaction.nextHighWater,
                                          &next)) {
      return poisonLocked(error);
    }
    generation = next.generation;
    highWater = next.highWater;
    selectedDescriptor = next;
    hasSelectedDescriptor = true;
    return 0;
  }

public:
  // A store is deliberately non-mountable until a logical payload backend
  // supplies the complete atomic namespace and metadata contracts.
  std::shared_ptr<DataFile> createFile(mode_t) override { return nullptr; }
  std::shared_ptr<Directory> createDirectory(mode_t) override {
    return nullptr;
  }
  std::shared_ptr<Symlink> createSymlink(std::string) override {
    return nullptr;
  }
  bool supportsExplicitMetadataMutation() const override { return false; }
  bool supportsRecordLocks() const override { return false; }

  int initialise(const char* profileName) {
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    if (!profileName) {
      return -EINVAL;
    }
    const std::string_view profile(profileName);
    const uint64_t profileSize = profile.size();
    if (profileSize > std::numeric_limits<uint32_t>::max()) {
      return -EOVERFLOW;
    }
    profileChecksum = profileLogV4Checksum(
      reinterpret_cast<const uint8_t*>(profile.data()),
      static_cast<size_t>(profileSize));
    profileLength = static_cast<uint32_t>(profileSize);
    const std::string stem = storageStem(profile);
    bootstrapName = stem + "-bootstrap";
    controlName = stem + "-control";
    arenaNames[0] = stem + "-arena-0";
    arenaNames[1] = stem + "-arena-1";

    if (int error = initialisePhysicalRootLocked()) {
      return error;
    }
    const int bootstrapStatus = lookupPhysicalFileLocked(bootstrapName);
    if (recoverInitialBootstrap && bootstrapStatus == 0) {
      // Inspect first while bootstrap is the only opened handle. Do not infer
      // recoverability from a missing name, an empty file, or a malformed
      // record: only the exact mirrored PREPARED pair, or R1/P1 with a fully
      // verified empty g=1 envelope, receives special treatment.
      bootstrap = makePhysicalFile(bootstrapName);
      if (int error = openFileLocked(bootstrap, &bootstrapOpen)) {
        return error;
      }
      if (int error = requireFixedSizeLocked(
            bootstrap, kProfileLogV4BootstrapSize)) {
        return error;
      }
      const auto disposition = classifyBootstrapLocked();
      if (disposition == ProfileLogV4BootstrapDisposition::Prepared) {
        if (int error = discardPreparedInitialBootstrapLocked()) {
          return error;
        }
        if (int error = initialiseFreshLocked()) {
          return error;
        }
      } else if (disposition ==
                 ProfileLogV4BootstrapDisposition::InitialGeneration) {
        for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
          if (int status = lookupPhysicalFileLocked(name)) {
            return status == -ENOENT ? -EIO : status;
          }
        }
        if (int error = closeInitialBootstrapInspectionLocked()) {
          return error;
        }
        if (int error = openInitialGenerationLocked()) {
          return error;
        }
        if (int error = completeInitialBootstrapLocked()) {
          return error;
        }
      } else if (disposition == ProfileLogV4BootstrapDisposition::Established) {
        for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
          if (int status = lookupPhysicalFileLocked(name)) {
            return status == -ENOENT ? -EIO : status;
          }
        }
        if (int error = closeInitialBootstrapInspectionLocked()) {
          return error;
        }
        if (int error = openEstablishedLocked()) {
          return error;
        }
      } else {
        return -EIO;
      }
    } else if (bootstrapStatus == -ENOENT) {
      for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
        if (int status = lookupPhysicalFileLocked(name); status != -ENOENT) {
          return status == 0 ? -EIO : status;
        }
      }
      if (int error = initialiseFreshLocked()) {
        return error;
      }
    } else {
      if (bootstrapStatus) {
        return bootstrapStatus;
      }
      for (const auto& name : {controlName, arenaNames[0], arenaNames[1]}) {
        if (int status = lookupPhysicalFileLocked(name)) {
          return status == -ENOENT ? -EIO : status;
        }
      }
      if (int error = openEstablishedLocked()) {
        return error;
      }
    }
    bootstrapComplete = true;
    return 0;
  }

  int prepareOPFSProfileRetirement(bool checkResources) override {
    int firstError = 0;
    {
      ProfileLeaseState::InternalOperation operation(*profileLeaseState);
      if (!operation) {
        firstError = operation.getError();
      } else {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        if (int error = stopAfterUnacknowledgedProxyLocked()) {
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE == 1
          if (!checkResources && controlledProxyCompletionFailureForTesting) {
            // This source-selected fault occurs after a known completed
            // manifest flush, not during an OPFS handle operation. The
            // terminal latch above has already abandoned every physical
            // wrapper and closed destructor proxying, so explicit
            // failure-retirement can retain the lease without issuing a
            // second proxy or converting the intentional disposition into a
            // cleanup failure. A normal drain still receives |error|.
            return 0;
          }
#endif
          return error;
        }
        firstError = closeAllFilesForRetirementLocked(!fatalError);
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
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
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

// Preserve the intentionally narrow public V4 experiment.  In particular,
// this class remains non-mountable and is the only one that implements the
// opaque C ABI below.
class ProfileLogV4ManifestBackend final : public ProfileLogV4Store {
public:
  ProfileLogV4ManifestBackend() = default;

  int readOPFSProfileLogV4Manifest(uint8_t* buffer,
                                   size_t capacity,
                                   size_t* size) override {
    return readManifest(buffer, capacity, size);
  }

  int commitOPFSProfileLogV4Manifest(const uint8_t* data, size_t size) override {
    return commitManifest(data, size);
  }
};

// The mountable V4 filesystem has a distinct physical stem from the opaque
// manifest experiment above. Its logical state is one strictly validated
// inode manifest, while regular-file data lives in immutable copy-on-write
// chunk records appended through ProfileLogV4Store::Transaction.
// The logical namespace and immutable data-record formats evolve
// independently. In particular, Schema-2 logical deltas still reference the
// original WFSV4DA1 chunk record format.
constexpr uint32_t kProfileLogV4FilesystemLogicalSchema1 = 1;
constexpr uint32_t kProfileLogV4FilesystemLogicalSchema2 = 2;
constexpr uint32_t kProfileLogV4FilesystemDataSchema1 = 1;
constexpr uint32_t kProfileLogV4FilesystemChunkShift = 16;
constexpr size_t kProfileLogV4FilesystemChunkSize =
  size_t(1) << kProfileLogV4FilesystemChunkShift;
constexpr size_t kProfileLogV4FilesystemHeaderSize = 128;
constexpr size_t kProfileLogV4FilesystemInodeSize = 112;
constexpr size_t kProfileLogV4FilesystemDirectoryEntrySize = 40;
constexpr size_t kProfileLogV4FilesystemExtentSize = 48;
constexpr size_t kProfileLogV4FilesystemDataHeaderSize = 96;
constexpr size_t kProfileLogV4FilesystemDeltaOperationSize = 32;
constexpr size_t kProfileLogV4FilesystemDeltaInodeHeaderSize = 128;
constexpr size_t kProfileLogV4FilesystemDeltaDirectoryEntrySize = 32;
constexpr size_t kProfileLogV4FilesystemDeltaExtentSize = 40;
constexpr size_t kProfileLogV4FilesystemNameMax = 255;
// A self-contained checkpoint is intentionally a bounded-retention building
// block, not the eventual scalable mutation format. It copies the current
// live tree into the next parity arena and lets the selector release the old
// arena only after both phase witnesses are durable. The cadence must be odd:
// an even period would always select the same generation-parity arena and
// could never reclaim the other one. Schema-2 delta records remove the
// full-namespace serialization work between these checkpoints.
constexpr uint64_t kProfileLogV4FilesystemCheckpointInterval =
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL
    ? WASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL
    : 31;
static_assert(kProfileLogV4FilesystemCheckpointInterval & 1);
constexpr std::array<uint8_t, 8> kProfileLogV4FilesystemMagic = {
  'W', 'F', 'S', 'V', '4', 'F', 'S', '1'};
constexpr std::array<uint8_t, 8> kProfileLogV4FilesystemDeltaMagic = {
  'W', 'F', 'S', 'V', '4', 'F', 'S', '2'};
constexpr std::array<uint8_t, 8> kProfileLogV4FilesystemDataMagic = {
  'W', 'F', 'S', 'V', '4', 'D', 'A', '1'};

enum class ProfileLogV4FilesystemInodeKind : uint32_t {
  Regular = 1,
  Directory = 2,
  Symlink = 3,
};

struct ProfileLogV4FilesystemExtent {
  uint64_t chunk = 0;
  uint32_t arena = 0;
  uint32_t payloadSize = 0;
  uint64_t offset = 0;
  uint64_t checksum = 0;
};

struct ProfileLogV4FilesystemInode {
  uint64_t id = 0;
  ProfileLogV4FilesystemInodeKind kind =
    ProfileLogV4FilesystemInodeKind::Regular;
  File::Metadata metadata = {};
  uint64_t size = 0;
  std::map<std::string, uint64_t> entries;
  std::map<uint64_t, ProfileLogV4FilesystemExtent> extents;
  std::string target;
  // This is runtime-only state for a file retained by an open descriptor
  // after unlink. It is deliberately not serialized, so a fresh profile
  // cannot resurrect a deleted path even if that descriptor later mutates it.
  std::map<uint64_t, std::vector<uint8_t>> volatileChunks;
};

struct ProfileLogV4FilesystemState {
  uint64_t generation = 0;
  uint64_t root = 0;
  uint64_t nextInode = 1;
  uint32_t deltaDepth = 0;
  std::map<uint64_t, ProfileLogV4FilesystemInode> inodes;
};

enum class ProfileLogV4FilesystemDeltaOpcode : uint32_t {
  Set = 1,
  Delete = 2,
};

struct ProfileLogV4FilesystemDeltaOperation {
  uint64_t inode = 0;
  ProfileLogV4FilesystemDeltaOpcode opcode =
    ProfileLogV4FilesystemDeltaOpcode::Set;
  ProfileLogV4FilesystemInode value;
};

struct ProfileLogV4FilesystemDelta {
  uint64_t generation = 0;
  uint64_t root = 0;
  uint64_t nextInode = 0;
  uint32_t depth = 0;
  ProfileLogV4ManifestReference parent = {};
  std::vector<ProfileLogV4FilesystemDeltaOperation> operations;
};

bool profileLogV4FilesystemCheckedAdd(size_t lhs, size_t rhs, size_t* result) {
  if (!result || lhs > std::numeric_limits<size_t>::max() - rhs) {
    return false;
  }
  *result = lhs + rhs;
  return true;
}

bool profileLogV4FilesystemCheckedMultiply(size_t lhs,
                                           size_t rhs,
                                           size_t* result) {
  if (!result || (lhs && rhs > std::numeric_limits<size_t>::max() / lhs)) {
    return false;
  }
  *result = lhs * rhs;
  return true;
}

bool profileLogV4FilesystemRange(size_t size,
                                 uint64_t offset,
                                 uint64_t length,
                                 size_t* resultOffset = nullptr,
                                 size_t* resultLength = nullptr) {
  if (offset > size || length > size - offset ||
      offset > std::numeric_limits<size_t>::max() ||
      length > std::numeric_limits<size_t>::max()) {
    return false;
  }
  if (resultOffset) {
    *resultOffset = static_cast<size_t>(offset);
  }
  if (resultLength) {
    *resultLength = static_cast<size_t>(length);
  }
  return true;
}

void profileLogV4FilesystemWriteU32(std::vector<uint8_t>& data,
                                    size_t offset,
                                    uint32_t value) {
  assert(offset <= data.size() && sizeof(value) <= data.size() - offset);
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

void profileLogV4FilesystemWriteU64(std::vector<uint8_t>& data,
                                    size_t offset,
                                    uint64_t value) {
  assert(offset <= data.size() && sizeof(value) <= data.size() - offset);
  for (size_t i = 0; i != sizeof(value); ++i) {
    data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

bool profileLogV4FilesystemReadU32(const uint8_t* data,
                                   size_t size,
                                   size_t offset,
                                   uint32_t* value) {
  if (!data || !value || offset > size || sizeof(*value) > size - offset) {
    return false;
  }
  *value = 0;
  for (size_t i = 0; i != sizeof(*value); ++i) {
    *value |= uint32_t(data[offset + i]) << (8 * i);
  }
  return true;
}

bool profileLogV4FilesystemReadU64(const uint8_t* data,
                                   size_t size,
                                   size_t offset,
                                   uint64_t* value) {
  if (!data || !value || offset > size || sizeof(*value) > size - offset) {
    return false;
  }
  *value = 0;
  for (size_t i = 0; i != sizeof(*value); ++i) {
    *value |= uint64_t(data[offset + i]) << (8 * i);
  }
  return true;
}

uint64_t profileLogV4FilesystemDoubleBits(double value) {
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

double profileLogV4FilesystemDoubleFromBits(uint64_t bits) {
  double value = 0;
  static_assert(sizeof(bits) == sizeof(value));
  memcpy(&value, &bits, sizeof(value));
  return value;
}

bool profileLogV4FilesystemValidName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.size() <= kProfileLogV4FilesystemNameMax &&
         name.find('/') == std::string::npos &&
         name.find('\0') == std::string::npos;
}

class ProfileLogV4FilesystemBackend;
class ProfileLogV4FilesystemDataFile;
class ProfileLogV4FilesystemDirectory;
class ProfileLogV4FilesystemSymlink;

class ProfileLogV4FilesystemBackend final : public ProfileLogV4Store {
  struct PendingChunk {
    uint64_t inode = 0;
    uint64_t chunk = 0;
    std::vector<uint8_t> bytes;
  };

  std::recursive_mutex filesystemMutex;
  ProfileLogV4FilesystemState state;
  std::map<uint64_t, ProfileLogV4FilesystemInode> orphans;
  // Linked children use WasmFS's ordinary dcache for object identity. An
  // unlinked object is retained only while an fd, CWD, or syscall-local
  // shared_ptr keeps its weak witness alive; this prevents journal/temp-file
  // churn from turning the backend's identity table into an unbounded heap.
  std::unordered_map<uint64_t, std::weak_ptr<File>> orphanFiles;
  bool rootExposed = false;
  bool logicalStateLoaded = false;
  int filesystemFatal = 0;

  int poisonFilesystemLocked(int error);
  int operationErrorLocked() const;
  int validateCurrentStateLocked();
  void reapOrphansLocked();
  int loadLogicalStateLocked();
  int parseLogicalStateLocked(const std::vector<uint8_t>& manifest,
                              uint64_t generation,
                              ProfileLogV4FilesystemState* result);
  int parseLogicalDeltaLocked(const std::vector<uint8_t>& manifest,
                              uint64_t generation,
                              ProfileLogV4FilesystemDelta* result);
  int validateLogicalStateLocked(const ProfileLogV4FilesystemState& value,
                                 uint64_t generation,
                                 bool validateExtents,
                                 uint64_t selectedGeneration = 0);
  int serializeLogicalStateLocked(const ProfileLogV4FilesystemState& value,
                                  uint64_t generation,
                                  std::vector<uint8_t>* manifest);
  int serializeLogicalDeltaLocked(
    const ProfileLogV4FilesystemState& value,
    uint64_t generation,
    const ProfileLogV4ManifestReference& parent,
    const std::vector<uint64_t>& changed,
    const std::vector<uint64_t>& deleted,
    std::vector<uint8_t>* manifest);
  int appendImmutableChunkLocked(
    Transaction& transaction,
    uint64_t inode,
    uint64_t chunk,
    const std::vector<uint8_t>& bytes,
    ProfileLogV4FilesystemExtent* result);
  int copyLiveCheckpointExtentsLocked(ProfileLogV4FilesystemState* value,
                                      Transaction& transaction);
  int commitLogicalStateLocked(const std::vector<uint64_t>& changed,
                               const std::vector<uint64_t>& deleted,
                               std::vector<PendingChunk> pending,
                               bool allowReclamation = true,
                               bool forceCheckpoint = false);
  int readExtentLocked(const ProfileLogV4FilesystemExtent& extent,
                       uint64_t inode,
                       uint64_t chunk,
                       uint64_t selectedGeneration,
                       std::vector<uint8_t>* bytes,
                       bool selectedAlreadyValidated = false,
                       uint64_t maxRecordGeneration = 0);
  int readChunkLocked(const ProfileLogV4FilesystemInode& inode,
                      uint64_t chunk,
                      uint64_t generation,
                      std::vector<uint8_t>* bytes);
  int getChunkForWriteLocked(const ProfileLogV4FilesystemInode& inode,
                             uint64_t chunk,
                             std::vector<uint8_t>* bytes);
  ProfileLogV4FilesystemInode* findInodeLocked(uint64_t id);
  const ProfileLogV4FilesystemInode* findInodeLocked(uint64_t id) const;
  std::shared_ptr<File> makeFileLocked(uint64_t id);
  int flushLocked();
  int commitMetadataLocked(uint64_t id, const File::Metadata& metadata);
  ssize_t writeFileLocked(uint64_t id,
                          const uint8_t* buffer,
                          size_t length,
                          off_t offset,
                          const File::Metadata& metadata);
  int resizeFileLocked(uint64_t id,
                       off_t size,
                       const File::Metadata& metadata);
  int commitNamespaceMutationLocked(
    const Directory::NamespaceMutation& mutation);

  friend class ProfileLogV4FilesystemDataFile;
  friend class ProfileLogV4FilesystemDirectory;
  friend class ProfileLogV4FilesystemSymlink;

public:
  ProfileLogV4FilesystemBackend() : ProfileLogV4Store("fs", true) {}

  bool supportsExplicitMetadataMutation() const override { return true; }
  bool requiresAtomicMetadataMutations() const override { return true; }
  bool requiresAtomicNamespaceMutations() const override { return true; }
  bool supportsRecordLocks() const override {
    // Chromium's database locks are process-owned fcntl locks. The browser
    // Web Lock lease excludes every other WasmFS profile instance, so the
    // single-process WasmFS implementation can provide their normal range,
    // any-descriptor-close, and F_GETLK semantics without pretending to lock
    // unleased or cross-origin OPFS writers.
    return profileLeaseState->supportsRecordLocks();
  }

  bool supportsReadOnlySharedMmap() const override {
    // A V4 file read is reconstructed from immutable copy-on-write extents.
    // WasmFS can only return a detached heap copy for mmap(), so it cannot
    // keep a MAP_SHARED view coherent across a later durable publication.
    return false;
  }

  int validateOperation() override {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
    return validateCurrentStateLocked();
  }

  int initialiseFilesystem(const char* profileName);
  std::shared_ptr<DataFile> createFile(mode_t mode) override;
  std::shared_ptr<Directory> createDirectory(mode_t mode) override;
  std::shared_ptr<Directory> createMountRootDirectory(mode_t mode) override;
  std::shared_ptr<Symlink> createSymlink(std::string target) override;
  int prepareOPFSProfileRetirement(bool checkResources) override;
  bool abandonFailedInitialisation() {
    return ProfileLogV4Store::abandonFailedInitialisation();
  }
};

class ProfileLogV4FilesystemDataFile final : public DataFile {
  ProfileLogV4FilesystemBackend* const filesystem;
  uint64_t inode = 0;

  void setInitialMetadata(const File::Metadata& metadata) {
    mode = metadata.mode;
    atime = metadata.atime;
    mtime = metadata.mtime;
    ctime = metadata.ctime;
  }

  off_t getSize() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return error;
    }
    const auto* value = filesystem->findInodeLocked(inode);
    if (!value || value->kind != ProfileLogV4FilesystemInodeKind::Regular) {
      return -ENOENT;
    }
    if (value->size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return -EOVERFLOW;
    }
    return static_cast<off_t>(value->size);
  }

  int open(oflags_t flags) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return error;
    }
    const auto* value = filesystem->findInodeLocked(inode);
    if (!value || value->kind != ProfileLogV4FilesystemInodeKind::Regular ||
        (flags != O_RDONLY && flags != O_WRONLY && flags != O_RDWR)) {
      return -ENOENT;
    }
    return 0;
  }

  int close() override { return 0; }

  ssize_t read(uint8_t* buffer, size_t length, off_t offset) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return error;
    }
    if (offset < 0 || (!buffer && length)) {
      return -EINVAL;
    }
    const auto* value = filesystem->findInodeLocked(inode);
    if (!value || value->kind != ProfileLogV4FilesystemInodeKind::Regular) {
      return -ENOENT;
    }
    const uint64_t start = static_cast<uint64_t>(offset);
    if (start >= value->size || !length) {
      return 0;
    }
    const uint64_t available = value->size - start;
    const size_t count = static_cast<size_t>(
      std::min<uint64_t>(length, available));
    if (count > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
      return -EFBIG;
    }
    size_t cursor = 0;
    while (cursor != count) {
      const uint64_t position = start + cursor;
      const uint64_t chunk = position >> kProfileLogV4FilesystemChunkShift;
      const size_t within = static_cast<size_t>(
        position & (kProfileLogV4FilesystemChunkSize - 1));
      const size_t bytes = std::min(
        kProfileLogV4FilesystemChunkSize - within, count - cursor);
      std::vector<uint8_t> chunkBytes;
      if (int error = filesystem->readChunkLocked(
            *value, chunk, filesystem->state.generation, &chunkBytes)) {
        return filesystem->poisonFilesystemLocked(error);
      }
      memcpy(buffer + cursor, chunkBytes.data() + within, bytes);
      cursor += bytes;
    }
    return static_cast<ssize_t>(count);
  }

  ssize_t write(const uint8_t*, size_t, off_t) override { return -ENOTSUP; }

  ssize_t writeWithMetadata(const uint8_t* buffer,
                            size_t length,
                            off_t offset,
                            const File::Metadata& metadata) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->writeFileLocked(inode, buffer, length, offset, metadata);
  }

  int setSize(off_t) override { return -ENOTSUP; }

  int setSizeWithMetadata(off_t size, const File::Metadata& metadata) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->resizeFileLocked(inode, size, metadata);
  }

  int flush() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->flushLocked();
  }

  int persistMetadata(const File::Metadata& metadata) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->commitMetadataLocked(inode, metadata);
  }

  friend class ProfileLogV4FilesystemBackend;

  ProfileLogV4FilesystemDataFile(ProfileLogV4FilesystemBackend* filesystem,
                                 uint64_t inode,
                                 const File::Metadata& metadata)
    : DataFile(metadata.mode, filesystem), filesystem(filesystem), inode(inode) {
    setInitialMetadata(metadata);
  }

  ProfileLogV4FilesystemDataFile(ProfileLogV4FilesystemBackend* filesystem,
                                 mode_t mode)
    : DataFile(mode, filesystem), filesystem(filesystem) {}

  void adopt(uint64_t value) {
    assert(!inode && value);
    inode = value;
  }

public:
  ino_t getIno() override {
    return inode ? static_cast<ino_t>(inode) : File::getIno();
  }

  uint64_t getInode() const { return inode; }
  bool isCandidate() const { return !inode; }
};

class ProfileLogV4FilesystemDirectory final : public Directory {
  ProfileLogV4FilesystemBackend* const filesystem;
  uint64_t inode = 0;

  void setInitialMetadata(const File::Metadata& metadata) {
    mode = metadata.mode;
    atime = metadata.atime;
    mtime = metadata.mtime;
    ctime = metadata.ctime;
  }

  MaybeFile getChildWithError(const std::string& name) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return MaybeFile(operation.getError());
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return MaybeFile(error);
    }
    const auto* directory = filesystem->findInodeLocked(inode);
    if (!directory || directory->kind !=
                        ProfileLogV4FilesystemInodeKind::Directory ||
        !profileLogV4FilesystemValidName(name)) {
      return MaybeFile(-ENOENT);
    }
    const auto child = directory->entries.find(name);
    if (child == directory->entries.end()) {
      return MaybeFile(std::shared_ptr<File>());
    }
    auto file = filesystem->makeFileLocked(child->second);
    return file ? MaybeFile(std::move(file)) : MaybeFile(-EIO);
  }

  std::shared_ptr<File> getChild(const std::string& name) override {
    auto result = getChildWithError(name);
    return result.getError() ? nullptr : result.getFile();
  }

  std::shared_ptr<DataFile> insertDataFile(const std::string&, mode_t) override {
    return nullptr;
  }
  std::shared_ptr<Directory> insertDirectory(const std::string&, mode_t) override {
    return nullptr;
  }
  std::shared_ptr<Symlink> insertSymlink(const std::string&,
                                         const std::string&) override {
    return nullptr;
  }
  int insertMove(const std::string&, std::shared_ptr<File>) override {
    return -ENOTSUP;
  }
  int removeChild(const std::string&) override { return -ENOTSUP; }

  int commitNamespaceMutation(const NamespaceMutation& mutation) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->commitNamespaceMutationLocked(mutation);
  }

  ssize_t getNumEntries() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return error;
    }
    const auto* directory = filesystem->findInodeLocked(inode);
    if (!directory || directory->kind !=
                        ProfileLogV4FilesystemInodeKind::Directory) {
      return -ENOENT;
    }
    return directory->entries.size() >
             static_cast<size_t>(std::numeric_limits<ssize_t>::max())
           ? -EOVERFLOW
           : static_cast<ssize_t>(directory->entries.size());
  }

  MaybeEntries getEntries() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return {operation.getError()};
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return {error};
    }
    const auto* directory = filesystem->findInodeLocked(inode);
    if (!directory || directory->kind !=
                        ProfileLogV4FilesystemInodeKind::Directory) {
      return {-ENOENT};
    }
    std::vector<Entry> entries;
    entries.reserve(directory->entries.size());
    for (const auto& [name, childID] : directory->entries) {
      const auto* child = filesystem->findInodeLocked(childID);
      if (!child) {
        return {-EIO};
      }
      FileKind kind = UnknownKind;
      switch (child->kind) {
        case ProfileLogV4FilesystemInodeKind::Regular:
          kind = DataFileKind;
          break;
        case ProfileLogV4FilesystemInodeKind::Directory:
          kind = DirectoryKind;
          break;
        case ProfileLogV4FilesystemInodeKind::Symlink:
          kind = SymlinkKind;
          break;
      }
      entries.push_back({name, kind, static_cast<ino_t>(childID)});
    }
    return {std::move(entries)};
  }

  int flush() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->flushLocked();
  }

  int persistMetadata(const File::Metadata& metadata) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->commitMetadataLocked(inode, metadata);
  }

  off_t getSize() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return error;
    }
    const auto* directory = filesystem->findInodeLocked(inode);
    return directory && directory->kind ==
                          ProfileLogV4FilesystemInodeKind::Directory
             ? 4096
             : -ENOENT;
  }

  std::string getName(std::shared_ptr<File> file) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return "";
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    const auto* directory = filesystem->findInodeLocked(inode);
    if (!directory || directory->kind !=
                        ProfileLogV4FilesystemInodeKind::Directory ||
        !file) {
      return "";
    }
    const uint64_t childID = static_cast<uint64_t>(file->getIno());
    for (const auto& [name, candidate] : directory->entries) {
      if (candidate == childID) {
        return name;
      }
    }
    return "";
  }

  bool maintainsFileIdentity() override { return false; }

  friend class ProfileLogV4FilesystemBackend;

  ProfileLogV4FilesystemDirectory(ProfileLogV4FilesystemBackend* filesystem,
                                  uint64_t inode,
                                  const File::Metadata& metadata)
    : Directory(metadata.mode, filesystem), filesystem(filesystem), inode(inode) {
    setInitialMetadata(metadata);
  }

  ProfileLogV4FilesystemDirectory(ProfileLogV4FilesystemBackend* filesystem,
                                  mode_t mode)
    : Directory(mode, filesystem), filesystem(filesystem) {}

  void adopt(uint64_t value) {
    assert(!inode && value);
    inode = value;
  }

public:
  ino_t getIno() override {
    return inode ? static_cast<ino_t>(inode) : File::getIno();
  }

  uint64_t getInode() const { return inode; }
  bool isCandidate() const { return !inode; }
};

class ProfileLogV4FilesystemSymlink final : public Symlink {
  ProfileLogV4FilesystemBackend* const filesystem;
  uint64_t inode = 0;
  std::string target;

  void setInitialMetadata(const File::Metadata& metadata) {
    mode = metadata.mode;
    atime = metadata.atime;
    mtime = metadata.mtime;
    ctime = metadata.ctime;
  }

  int persistMetadata(const File::Metadata& metadata) override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    return filesystem->commitMetadataLocked(inode, metadata);
  }

  off_t getSize() override {
    ProfileLeaseState::InternalOperation operation(*filesystem->profileLeaseState);
    if (!operation) {
      return operation.getError();
    }
    std::lock_guard<std::recursive_mutex> lock(filesystem->filesystemMutex);
    if (int error = filesystem->validateCurrentStateLocked()) {
      return error;
    }
    const auto* symlink = filesystem->findInodeLocked(inode);
    if (!symlink || symlink->kind !=
                      ProfileLogV4FilesystemInodeKind::Symlink ||
        symlink->target.size() >
          static_cast<size_t>(std::numeric_limits<off_t>::max())) {
      return -ENOENT;
    }
    return static_cast<off_t>(symlink->target.size());
  }

  friend class ProfileLogV4FilesystemBackend;

  ProfileLogV4FilesystemSymlink(ProfileLogV4FilesystemBackend* filesystem,
                                uint64_t inode,
                                std::string target,
                                const File::Metadata& metadata)
    : Symlink(filesystem),
      filesystem(filesystem),
      inode(inode),
      target(std::move(target)) {
    setInitialMetadata(metadata);
  }

  ProfileLogV4FilesystemSymlink(ProfileLogV4FilesystemBackend* filesystem,
                                std::string target)
    : Symlink(filesystem), filesystem(filesystem), target(std::move(target)) {}

  void adopt(uint64_t value) {
    assert(!inode && value);
    inode = value;
  }

public:
  std::string getTarget() const override { return target; }
  ino_t getIno() override {
    return inode ? static_cast<ino_t>(inode) : File::getIno();
  }

  uint64_t getInode() const { return inode; }
  bool isCandidate() const { return !inode; }
};

int ProfileLogV4FilesystemBackend::poisonFilesystemLocked(int error) {
  if (error >= 0) {
    error = -EIO;
  }
  if (!filesystemFatal) {
    filesystemFatal = error;
  }
  return filesystemFatal;
}

int ProfileLogV4FilesystemBackend::operationErrorLocked() const {
  return filesystemFatal ? filesystemFatal : logicalStateLoaded ? 0 : -ESHUTDOWN;
}

void ProfileLogV4FilesystemBackend::reapOrphansLocked() {
  for (auto it = orphans.begin(); it != orphans.end();) {
    const auto witness = orphanFiles.find(it->first);
    if (witness != orphanFiles.end() && !witness->second.expired()) {
      ++it;
      continue;
    }
    if (witness != orphanFiles.end()) {
      orphanFiles.erase(witness);
    }
    it = orphans.erase(it);
  }
}

int ProfileLogV4FilesystemBackend::validateCurrentStateLocked() {
  if (int error = operationErrorLocked()) {
    return error;
  }
  reapOrphansLocked();
  if (!logicalStateLoaded || !state.generation ||
      (state.generation == 1 &&
       (state.root || state.nextInode != 1 || !state.inodes.empty() ||
        !orphans.empty() || !orphanFiles.empty()))) {
    return poisonFilesystemLocked(-EIO);
  }
  // Preserve the live fail-closed boundary: before serving cached namespace
  // state, reauthenticate the selected outer payload.  This is intentionally
  // cheaper than the schema-1 path because it does not reconstruct or
  // globally validate the logical tree on every operation; ordinary schema-2
  // commits carry only their changed inode postimages.  The selected full
  // checkpoint is still checksummed, as required to reject same-origin OPFS
  // corruption before it can be observed through cached metadata.
  size_t manifestSize = 0;
  uint64_t manifestGeneration = 0;
  if (int error = readManifest(
        nullptr, 0, &manifestSize, &manifestGeneration)) {
    return poisonFilesystemLocked(error);
  }
  if (manifestGeneration != state.generation ||
      ((manifestGeneration == 1) != (manifestSize == 0))) {
    return poisonFilesystemLocked(-EIO);
  }
  return 0;
}

ProfileLogV4FilesystemInode*
ProfileLogV4FilesystemBackend::findInodeLocked(uint64_t id) {
  if (auto found = state.inodes.find(id); found != state.inodes.end()) {
    return &found->second;
  }
  if (auto found = orphans.find(id); found != orphans.end()) {
    return &found->second;
  }
  return nullptr;
}

const ProfileLogV4FilesystemInode*
ProfileLogV4FilesystemBackend::findInodeLocked(uint64_t id) const {
  if (auto found = state.inodes.find(id); found != state.inodes.end()) {
    return &found->second;
  }
  if (auto found = orphans.find(id); found != orphans.end()) {
    return &found->second;
  }
  return nullptr;
}

std::shared_ptr<File> ProfileLogV4FilesystemBackend::makeFileLocked(
  uint64_t id) {
  if (!id || id > static_cast<uint64_t>(std::numeric_limits<ino_t>::max())) {
    return nullptr;
  }
  const auto* inode = findInodeLocked(id);
  if (!inode) {
    return nullptr;
  }
  std::shared_ptr<File> file;
  switch (inode->kind) {
    case ProfileLogV4FilesystemInodeKind::Regular:
      file = std::shared_ptr<ProfileLogV4FilesystemDataFile>(
        new ProfileLogV4FilesystemDataFile(this, id, inode->metadata));
      break;
    case ProfileLogV4FilesystemInodeKind::Directory:
      file = std::shared_ptr<ProfileLogV4FilesystemDirectory>(
        new ProfileLogV4FilesystemDirectory(this, id, inode->metadata));
      break;
    case ProfileLogV4FilesystemInodeKind::Symlink:
      file = std::shared_ptr<ProfileLogV4FilesystemSymlink>(
        new ProfileLogV4FilesystemSymlink(
          this, id, inode->target, inode->metadata));
      break;
  }
  return file;
}

int ProfileLogV4FilesystemBackend::initialiseFilesystem(const char* profileName) {
  if (int error = initialise(profileName)) {
    return error;
  }
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return operation.getError();
  }
  std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
  return loadLogicalStateLocked();
}

std::shared_ptr<DataFile> ProfileLogV4FilesystemBackend::createFile(mode_t mode) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
  if (validateCurrentStateLocked()) {
    return nullptr;
  }
  return std::shared_ptr<ProfileLogV4FilesystemDataFile>(
    new ProfileLogV4FilesystemDataFile(this, mode));
}

std::shared_ptr<Symlink> ProfileLogV4FilesystemBackend::createSymlink(
  std::string target) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
  if (validateCurrentStateLocked() || target.find('\0') != std::string::npos) {
    return nullptr;
  }
  return std::shared_ptr<ProfileLogV4FilesystemSymlink>(
    new ProfileLogV4FilesystemSymlink(this, std::move(target)));
}

std::shared_ptr<Directory> ProfileLogV4FilesystemBackend::createDirectory(
  mode_t mode) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
  if (validateCurrentStateLocked()) {
    return nullptr;
  }

  // Generic atomic namespace creation asks for a private candidate only after
  // the one mount root has been exposed. Do not let that candidate factory
  // double as the public mount-root path.
  if (!rootExposed || !state.root) {
    return nullptr;
  }
  return std::shared_ptr<ProfileLogV4FilesystemDirectory>(
    new ProfileLogV4FilesystemDirectory(this, mode));
}

std::shared_ptr<Directory>
ProfileLogV4FilesystemBackend::createMountRootDirectory(mode_t mode) {
  ProfileLeaseState::InternalOperation operation(*profileLeaseState);
  if (!operation) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
  if (validateCurrentStateLocked() || rootExposed) {
    return nullptr;
  }

  if (!state.root) {
    auto root = std::shared_ptr<ProfileLogV4FilesystemDirectory>(
      new ProfileLogV4FilesystemDirectory(this, mode));
    auto metadata = root->locked().getMetadata();
    state.root = 1;
    state.nextInode = 2;
    state.inodes.emplace(
      1,
      ProfileLogV4FilesystemInode{1,
                                   ProfileLogV4FilesystemInodeKind::Directory,
                                   metadata,
                                   0,
                                   {},
                                   {},
                                   {},
                                   {}});
    if (commitLogicalStateLocked({1}, {}, {}, true, true)) {
      return nullptr;
    }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST && \
  WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT
    wasmfs_opfs_profile_log_v4_test_maybe_interrupt(9);
#endif
    root->adopt(1);
    rootExposed = true;
    return root;
  }
  auto root = makeFileLocked(state.root);
  if (!root || !root->is<ProfileLogV4FilesystemDirectory>()) {
    poisonFilesystemLocked(-EIO);
    return nullptr;
  }
  rootExposed = true;
  return root->cast<Directory>();
}

mode_t profileLogV4FilesystemExpectedMode(
  ProfileLogV4FilesystemInodeKind kind) {
  switch (kind) {
    case ProfileLogV4FilesystemInodeKind::Regular:
      return S_IFREG;
    case ProfileLogV4FilesystemInodeKind::Directory:
      return S_IFDIR;
    case ProfileLogV4FilesystemInodeKind::Symlink:
      return S_IFLNK;
  }
  return 0;
}

bool profileLogV4FilesystemValidMetadata(
  const File::Metadata& metadata,
  ProfileLogV4FilesystemInodeKind kind) {
  return (metadata.mode & S_IFMT) == profileLogV4FilesystemExpectedMode(kind) &&
         std::isfinite(metadata.atime) && std::isfinite(metadata.mtime) &&
         std::isfinite(metadata.ctime);
}

std::array<uint8_t, kProfileLogV4FilesystemDataHeaderSize>
profileLogV4FilesystemMakeDataHeader(uint64_t generation,
                                     uint64_t inode,
                                     uint64_t chunk,
                                     const uint8_t* payload,
                                     size_t size) {
  std::array<uint8_t, kProfileLogV4FilesystemDataHeaderSize> header = {};
  std::copy(kProfileLogV4FilesystemDataMagic.begin(),
            kProfileLogV4FilesystemDataMagic.end(),
            header.begin());
  writeProfileLogV4U32(header, 8, kProfileLogV4FilesystemDataSchema1);
  writeProfileLogV4U32(header, 12, kProfileLogV4FilesystemDataHeaderSize);
  writeProfileLogV4U64(header, 16, generation);
  writeProfileLogV4U64(header, 24, inode);
  writeProfileLogV4U64(header, 32, chunk);
  writeProfileLogV4U32(header, 40, size);
  writeProfileLogV4U32(header, 44, 0);
  writeProfileLogV4U64(header, 48, profileLogV4Checksum(payload, size));
  writeProfileLogV4U64(
    header, 56, profileLogV4ChecksumWithZeroedRange(header, 56, 8));
  return header;
}

bool profileLogV4FilesystemParseDataHeader(
  const std::array<uint8_t, kProfileLogV4FilesystemDataHeaderSize>& header,
  uint64_t* generation,
  uint64_t* inode,
  uint64_t* chunk,
  uint32_t* payloadSize,
  uint64_t* payloadChecksum) {
  if (!generation || !inode || !chunk || !payloadSize || !payloadChecksum ||
      !std::equal(kProfileLogV4FilesystemDataMagic.begin(),
                  kProfileLogV4FilesystemDataMagic.end(),
                  header.begin()) ||
      readProfileLogV4U32(header, 8) !=
        kProfileLogV4FilesystemDataSchema1 ||
      readProfileLogV4U32(header, 12) !=
        kProfileLogV4FilesystemDataHeaderSize ||
      readProfileLogV4U32(header, 44) != 0 ||
      readProfileLogV4U64(header, 56) !=
        profileLogV4ChecksumWithZeroedRange(header, 56, 8) ||
      !profileLogV4HasZeroTail(header, 64)) {
    return false;
  }
  *generation = readProfileLogV4U64(header, 16);
  *inode = readProfileLogV4U64(header, 24);
  *chunk = readProfileLogV4U64(header, 32);
  *payloadSize = readProfileLogV4U32(header, 40);
  *payloadChecksum = readProfileLogV4U64(header, 48);
  return *generation != 0;
}

// A Schema-2 Set operation stores a complete post-image for exactly one
// inode. The tables use offsets local to this blob, so a one-file metadata
// change never requires rewriting unrelated inode, directory, or extent
// tables from the Schema-1 checkpoint.
int profileLogV4FilesystemSerializeDeltaInode(
  const ProfileLogV4FilesystemInode& inode,
  std::vector<uint8_t>* blob) {
  if (!blob || !inode.id ||
      inode.id > static_cast<uint64_t>(std::numeric_limits<ino_t>::max()) ||
      !profileLogV4FilesystemValidMetadata(inode.metadata, inode.kind) ||
      !inode.volatileChunks.empty()) {
    return -EIO;
  }
  if ((inode.kind == ProfileLogV4FilesystemInodeKind::Directory &&
       (inode.size || !inode.extents.empty() || !inode.target.empty())) ||
      (inode.kind == ProfileLogV4FilesystemInodeKind::Regular &&
       (!inode.entries.empty() || !inode.target.empty() ||
        inode.size > kProfileLogV4MaxSafeOffset)) ||
      (inode.kind == ProfileLogV4FilesystemInodeKind::Symlink &&
       (inode.size || !inode.entries.empty() || !inode.extents.empty() ||
        inode.target.find('\0') != std::string::npos))) {
    return -EIO;
  }

  std::vector<uint8_t> strings;
  if (inode.kind == ProfileLogV4FilesystemInodeKind::Directory) {
    for (const auto& [name, child] : inode.entries) {
      if (!child || !profileLogV4FilesystemValidName(name) ||
          strings.size() > kProfileLogV4MaxSafeOffset - name.size()) {
        return -EIO;
      }
      strings.insert(strings.end(), name.begin(), name.end());
    }
  } else if (inode.kind == ProfileLogV4FilesystemInodeKind::Symlink) {
    if (inode.target.size() > kProfileLogV4MaxSafeOffset - strings.size()) {
      return -EFBIG;
    }
    strings.insert(strings.end(), inode.target.begin(), inode.target.end());
  }

  if (inode.kind == ProfileLogV4FilesystemInodeKind::Regular) {
    uint64_t previousChunk = 0;
    bool haveChunk = false;
    for (const auto& [chunk, extent] : inode.extents) {
      if ((haveChunk && chunk <= previousChunk) || extent.chunk != chunk ||
          extent.arena >= 2 ||
          extent.payloadSize != kProfileLogV4FilesystemChunkSize ||
          !inode.size ||
          chunk > ((inode.size - 1) >> kProfileLogV4FilesystemChunkShift)) {
        return -EIO;
      }
      previousChunk = chunk;
      haveChunk = true;
    }
  }

  size_t entryBytes = 0;
  size_t extentBytes = 0;
  size_t extentOffset = 0;
  size_t stringOffset = 0;
  size_t total = 0;
  if (!profileLogV4FilesystemCheckedMultiply(
        inode.entries.size(), kProfileLogV4FilesystemDeltaDirectoryEntrySize,
        &entryBytes) ||
      !profileLogV4FilesystemCheckedMultiply(
        inode.extents.size(), kProfileLogV4FilesystemDeltaExtentSize,
        &extentBytes) ||
      !profileLogV4FilesystemCheckedAdd(
        kProfileLogV4FilesystemDeltaInodeHeaderSize, entryBytes,
        &extentOffset) ||
      !profileLogV4FilesystemCheckedAdd(extentOffset, extentBytes,
                                         &stringOffset) ||
      !profileLogV4FilesystemCheckedAdd(stringOffset, strings.size(),
                                         &total)) {
    return -EFBIG;
  }
  blob->assign(total, 0);
  profileLogV4FilesystemWriteU64(*blob, 0, inode.id);
  profileLogV4FilesystemWriteU32(
    *blob, 8, static_cast<uint32_t>(inode.kind));
  profileLogV4FilesystemWriteU32(*blob, 16, inode.metadata.mode);
  profileLogV4FilesystemWriteU64(
    *blob, 24, profileLogV4FilesystemDoubleBits(inode.metadata.atime));
  profileLogV4FilesystemWriteU64(
    *blob, 32, profileLogV4FilesystemDoubleBits(inode.metadata.mtime));
  profileLogV4FilesystemWriteU64(
    *blob, 40, profileLogV4FilesystemDoubleBits(inode.metadata.ctime));
  profileLogV4FilesystemWriteU64(*blob, 48, inode.size);
  profileLogV4FilesystemWriteU64(
    *blob, 56, kProfileLogV4FilesystemDeltaInodeHeaderSize);
  profileLogV4FilesystemWriteU64(*blob, 64, inode.entries.size());
  profileLogV4FilesystemWriteU64(*blob, 72, extentOffset);
  profileLogV4FilesystemWriteU64(*blob, 80, inode.extents.size());
  profileLogV4FilesystemWriteU64(*blob, 88, 0);
  profileLogV4FilesystemWriteU64(*blob, 96,
                                  inode.kind ==
                                      ProfileLogV4FilesystemInodeKind::Symlink
                                    ? inode.target.size()
                                    : 0);

  size_t entryCursor = kProfileLogV4FilesystemDeltaInodeHeaderSize;
  size_t stringCursor = 0;
  for (const auto& [name, child] : inode.entries) {
    profileLogV4FilesystemWriteU64(*blob, entryCursor, child);
    profileLogV4FilesystemWriteU64(*blob, entryCursor + 8, stringCursor);
    profileLogV4FilesystemWriteU32(*blob, entryCursor + 16, name.size());
    entryCursor += kProfileLogV4FilesystemDeltaDirectoryEntrySize;
    stringCursor += name.size();
  }
  size_t extentCursor = extentOffset;
  for (const auto& [chunk, extent] : inode.extents) {
    profileLogV4FilesystemWriteU64(*blob, extentCursor, chunk);
    profileLogV4FilesystemWriteU32(*blob, extentCursor + 8, extent.arena);
    profileLogV4FilesystemWriteU32(
      *blob, extentCursor + 12, extent.payloadSize);
    profileLogV4FilesystemWriteU64(*blob, extentCursor + 16, extent.offset);
    profileLogV4FilesystemWriteU64(*blob, extentCursor + 24, extent.checksum);
    extentCursor += kProfileLogV4FilesystemDeltaExtentSize;
  }
  if (!strings.empty()) {
    memcpy(blob->data() + stringOffset, strings.data(), strings.size());
  }
  return 0;
}

int profileLogV4FilesystemParseDeltaInode(
  const uint8_t* data,
  size_t size,
  uint64_t expectedID,
  ProfileLogV4FilesystemInode* result) {
  if (!data || !result || !expectedID ||
      size < kProfileLogV4FilesystemDeltaInodeHeaderSize) {
    return -EIO;
  }
  const auto read32 = [&](size_t offset, uint32_t* value) {
    return profileLogV4FilesystemReadU32(data, size, offset, value);
  };
  const auto read64 = [&](size_t offset, uint64_t* value) {
    return profileLogV4FilesystemReadU64(data, size, offset, value);
  };
  uint64_t id = 0;
  uint32_t kindRaw = 0;
  uint32_t reserved12 = 0;
  uint32_t mode = 0;
  uint32_t reserved20 = 0;
  uint64_t atime = 0;
  uint64_t mtime = 0;
  uint64_t ctime = 0;
  uint64_t inodeSize = 0;
  uint64_t entryOffset64 = 0;
  uint64_t entryCount64 = 0;
  uint64_t extentOffset64 = 0;
  uint64_t extentCount64 = 0;
  uint64_t targetOffset = 0;
  uint64_t targetSize64 = 0;
  uint64_t reserved104 = 0;
  uint64_t reserved112 = 0;
  uint64_t reserved120 = 0;
  if (!read64(0, &id) || !read32(8, &kindRaw) ||
      !read32(12, &reserved12) || !read32(16, &mode) ||
      !read32(20, &reserved20) || !read64(24, &atime) ||
      !read64(32, &mtime) || !read64(40, &ctime) ||
      !read64(48, &inodeSize) || !read64(56, &entryOffset64) ||
      !read64(64, &entryCount64) || !read64(72, &extentOffset64) ||
      !read64(80, &extentCount64) || !read64(88, &targetOffset) ||
      !read64(96, &targetSize64) || !read64(104, &reserved104) ||
      !read64(112, &reserved112) || !read64(120, &reserved120) ||
      id != expectedID ||
      id > static_cast<uint64_t>(std::numeric_limits<ino_t>::max()) ||
      reserved12 || reserved20 || reserved104 || reserved112 || reserved120 ||
      entryCount64 > std::numeric_limits<size_t>::max() ||
      extentCount64 > std::numeric_limits<size_t>::max()) {
    return -EIO;
  }
  ProfileLogV4FilesystemInodeKind kind;
  switch (kindRaw) {
    case static_cast<uint32_t>(ProfileLogV4FilesystemInodeKind::Regular):
      kind = ProfileLogV4FilesystemInodeKind::Regular;
      break;
    case static_cast<uint32_t>(ProfileLogV4FilesystemInodeKind::Directory):
      kind = ProfileLogV4FilesystemInodeKind::Directory;
      break;
    case static_cast<uint32_t>(ProfileLogV4FilesystemInodeKind::Symlink):
      kind = ProfileLogV4FilesystemInodeKind::Symlink;
      break;
    default:
      return -EIO;
  }
  const File::Metadata metadata = {
    static_cast<mode_t>(mode),
    profileLogV4FilesystemDoubleFromBits(atime),
    profileLogV4FilesystemDoubleFromBits(mtime),
    profileLogV4FilesystemDoubleFromBits(ctime)};
  if (!profileLogV4FilesystemValidMetadata(metadata, kind) ||
      inodeSize > kProfileLogV4MaxSafeOffset) {
    return -EIO;
  }
  const size_t entryCount = static_cast<size_t>(entryCount64);
  const size_t extentCount = static_cast<size_t>(extentCount64);
  size_t entryBytes = 0;
  size_t extentBytes = 0;
  size_t expectedExtentOffset = 0;
  size_t expectedStringOffset = 0;
  if (!profileLogV4FilesystemCheckedMultiply(
        entryCount, kProfileLogV4FilesystemDeltaDirectoryEntrySize,
        &entryBytes) ||
      !profileLogV4FilesystemCheckedMultiply(
        extentCount, kProfileLogV4FilesystemDeltaExtentSize, &extentBytes) ||
      !profileLogV4FilesystemCheckedAdd(
        kProfileLogV4FilesystemDeltaInodeHeaderSize, entryBytes,
        &expectedExtentOffset) ||
      !profileLogV4FilesystemCheckedAdd(expectedExtentOffset, extentBytes,
                                         &expectedStringOffset) ||
      entryOffset64 != kProfileLogV4FilesystemDeltaInodeHeaderSize ||
      extentOffset64 != expectedExtentOffset ||
      expectedStringOffset > size) {
    return -EIO;
  }
  const size_t stringSize = size - expectedStringOffset;
  ProfileLogV4FilesystemInode inode;
  inode.id = id;
  inode.kind = kind;
  inode.metadata = metadata;
  inode.size = inodeSize;

  if (kind == ProfileLogV4FilesystemInodeKind::Directory) {
    if (inodeSize || extentCount || targetOffset || targetSize64) {
      return -EIO;
    }
    size_t stringCursor = 0;
    std::string previousName;
    for (size_t index = 0; index != entryCount; ++index) {
      const size_t offset = kProfileLogV4FilesystemDeltaInodeHeaderSize +
                            index *
                              kProfileLogV4FilesystemDeltaDirectoryEntrySize;
      uint64_t child = 0;
      uint64_t nameOffset = 0;
      uint32_t nameSize = 0;
      uint32_t reserved20 = 0;
      uint64_t reserved24 = 0;
      if (!read64(offset, &child) || !read64(offset + 8, &nameOffset) ||
          !read32(offset + 16, &nameSize) || !read32(offset + 20, &reserved20) ||
          !read64(offset + 24, &reserved24) || !child || reserved20 ||
          reserved24 || nameOffset != stringCursor ||
          nameSize > stringSize - stringCursor) {
        return -EIO;
      }
      std::string name(reinterpret_cast<const char*>(
                         data + expectedStringOffset + stringCursor),
                       nameSize);
      if (!profileLogV4FilesystemValidName(name) ||
          (index && !(previousName < name)) ||
          !inode.entries.emplace(name, child).second) {
        return -EIO;
      }
      previousName = std::move(name);
      stringCursor += nameSize;
    }
    if (stringCursor != stringSize) {
      return -EIO;
    }
  } else if (kind == ProfileLogV4FilesystemInodeKind::Regular) {
    if (entryCount || targetOffset || targetSize64 || stringSize) {
      return -EIO;
    }
    uint64_t previousChunk = 0;
    for (size_t index = 0; index != extentCount; ++index) {
      const size_t offset = expectedExtentOffset +
                            index * kProfileLogV4FilesystemDeltaExtentSize;
      ProfileLogV4FilesystemExtent extent;
      uint64_t reserved32 = 0;
      if (!read64(offset, &extent.chunk) || !read32(offset + 8, &extent.arena) ||
          !read32(offset + 12, &extent.payloadSize) ||
          !read64(offset + 16, &extent.offset) ||
          !read64(offset + 24, &extent.checksum) ||
          !read64(offset + 32, &reserved32) || reserved32 ||
          extent.arena >= 2 ||
          extent.payloadSize != kProfileLogV4FilesystemChunkSize ||
          !inodeSize ||
          (index && extent.chunk <= previousChunk) ||
          extent.chunk >
            ((inodeSize - 1) >> kProfileLogV4FilesystemChunkShift) ||
          !inode.extents.emplace(extent.chunk, extent).second) {
        return -EIO;
      }
      previousChunk = extent.chunk;
    }
  } else {
    if (inodeSize || entryCount || extentCount || targetOffset ||
        targetSize64 != stringSize ||
        targetSize64 > std::numeric_limits<size_t>::max()) {
      return -EIO;
    }
    inode.target.assign(reinterpret_cast<const char*>(data + expectedStringOffset),
                        stringSize);
    if (inode.target.find('\0') != std::string::npos) {
      return -EIO;
    }
  }
  *result = std::move(inode);
  return 0;
}

int ProfileLogV4FilesystemBackend::serializeLogicalStateLocked(
  const ProfileLogV4FilesystemState& value,
  uint64_t generation,
  std::vector<uint8_t>* manifest) {
  if (!manifest) {
    return -EINVAL;
  }
  if (int error = validateLogicalStateLocked(value, generation, false)) {
    return error;
  }

  struct InodeOffsets {
    uint64_t entryFirst = 0;
    uint64_t entryCount = 0;
    uint64_t extentFirst = 0;
    uint64_t extentCount = 0;
    uint64_t targetOffset = 0;
    uint64_t targetLength = 0;
  };
  struct EntryOutput {
    uint64_t parent;
    uint64_t child;
    uint64_t nameOffset;
    uint32_t nameLength;
  };

  std::map<uint64_t, InodeOffsets> offsets;
  std::vector<EntryOutput> entries;
  std::vector<ProfileLogV4FilesystemExtent> extents;
  std::vector<uint64_t> extentInodes;
  std::vector<uint8_t> strings;
  for (const auto& [id, inode] : value.inodes) {
    auto& inodeOffsets = offsets[id];
    if (inode.kind == ProfileLogV4FilesystemInodeKind::Directory) {
      inodeOffsets.entryFirst = entries.size();
      inodeOffsets.entryCount = inode.entries.size();
      for (const auto& [name, child] : inode.entries) {
        if (name.size() > std::numeric_limits<uint32_t>::max() ||
            strings.size() > kProfileLogV4MaxSafeOffset - name.size()) {
          return -EFBIG;
        }
        entries.push_back({id,
                           child,
                           static_cast<uint64_t>(strings.size()),
                           static_cast<uint32_t>(name.size())});
        strings.insert(strings.end(), name.begin(), name.end());
      }
    }
    if (inode.kind == ProfileLogV4FilesystemInodeKind::Regular) {
      inodeOffsets.extentFirst = extents.size();
      inodeOffsets.extentCount = inode.extents.size();
      for (const auto& [_, extent] : inode.extents) {
        extents.push_back(extent);
        extentInodes.push_back(id);
      }
    }
    if (inode.kind == ProfileLogV4FilesystemInodeKind::Symlink) {
      if (strings.size() > kProfileLogV4MaxSafeOffset - inode.target.size()) {
        return -EFBIG;
      }
      inodeOffsets.targetOffset = strings.size();
      inodeOffsets.targetLength = inode.target.size();
      strings.insert(strings.end(), inode.target.begin(), inode.target.end());
    }
  }

  size_t inodeBytes = 0;
  size_t entryBytes = 0;
  size_t extentBytes = 0;
  size_t entryOffset = 0;
  size_t extentOffset = 0;
  size_t stringOffset = 0;
  size_t totalSize = 0;
  if (!profileLogV4FilesystemCheckedMultiply(
        value.inodes.size(), kProfileLogV4FilesystemInodeSize, &inodeBytes) ||
      !profileLogV4FilesystemCheckedMultiply(
        entries.size(), kProfileLogV4FilesystemDirectoryEntrySize, &entryBytes) ||
      !profileLogV4FilesystemCheckedMultiply(
        extents.size(), kProfileLogV4FilesystemExtentSize, &extentBytes) ||
      !profileLogV4FilesystemCheckedAdd(
        kProfileLogV4FilesystemHeaderSize, inodeBytes, &entryOffset) ||
      !profileLogV4FilesystemCheckedAdd(entryOffset, entryBytes, &extentOffset) ||
      !profileLogV4FilesystemCheckedAdd(extentOffset, extentBytes, &stringOffset) ||
      !profileLogV4FilesystemCheckedAdd(stringOffset, strings.size(), &totalSize)) {
    return -EFBIG;
  }

  manifest->assign(totalSize, 0);
  std::copy(kProfileLogV4FilesystemMagic.begin(),
            kProfileLogV4FilesystemMagic.end(),
            manifest->begin());
  profileLogV4FilesystemWriteU32(
    *manifest, 8, kProfileLogV4FilesystemLogicalSchema1);
  profileLogV4FilesystemWriteU32(
    *manifest, 12, kProfileLogV4FilesystemHeaderSize);
  profileLogV4FilesystemWriteU64(*manifest, 16, generation);
  profileLogV4FilesystemWriteU64(*manifest, 24, value.root);
  profileLogV4FilesystemWriteU64(*manifest, 32, value.nextInode);
  profileLogV4FilesystemWriteU32(
    *manifest, 40, kProfileLogV4FilesystemChunkShift);
  profileLogV4FilesystemWriteU32(*manifest, 44, 0);
  profileLogV4FilesystemWriteU64(
    *manifest, 48, kProfileLogV4FilesystemHeaderSize);
  profileLogV4FilesystemWriteU64(*manifest, 56, value.inodes.size());
  profileLogV4FilesystemWriteU64(*manifest, 64, entryOffset);
  profileLogV4FilesystemWriteU64(*manifest, 72, entries.size());
  profileLogV4FilesystemWriteU64(*manifest, 80, extentOffset);
  profileLogV4FilesystemWriteU64(*manifest, 88, extents.size());
  profileLogV4FilesystemWriteU64(*manifest, 96, stringOffset);
  profileLogV4FilesystemWriteU64(*manifest, 104, strings.size());

  size_t inodeCursor = kProfileLogV4FilesystemHeaderSize;
  for (const auto& [id, inode] : value.inodes) {
    const auto& inodeOffsets = offsets[id];
    profileLogV4FilesystemWriteU64(*manifest, inodeCursor, id);
    profileLogV4FilesystemWriteU32(
      *manifest, inodeCursor + 8, static_cast<uint32_t>(inode.kind));
    profileLogV4FilesystemWriteU32(
      *manifest, inodeCursor + 16, inode.metadata.mode);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 24,
      profileLogV4FilesystemDoubleBits(inode.metadata.atime));
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 32,
      profileLogV4FilesystemDoubleBits(inode.metadata.mtime));
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 40,
      profileLogV4FilesystemDoubleBits(inode.metadata.ctime));
    profileLogV4FilesystemWriteU64(*manifest, inodeCursor + 48, inode.size);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 56, inodeOffsets.entryFirst);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 64, inodeOffsets.entryCount);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 72, inodeOffsets.extentFirst);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 80, inodeOffsets.extentCount);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 88, inodeOffsets.targetOffset);
    profileLogV4FilesystemWriteU64(
      *manifest, inodeCursor + 96, inodeOffsets.targetLength);
    inodeCursor += kProfileLogV4FilesystemInodeSize;
  }

  size_t entryCursor = entryOffset;
  for (const auto& entry : entries) {
    profileLogV4FilesystemWriteU64(*manifest, entryCursor, entry.parent);
    profileLogV4FilesystemWriteU64(*manifest, entryCursor + 8, entry.child);
    profileLogV4FilesystemWriteU64(*manifest, entryCursor + 16, entry.nameOffset);
    profileLogV4FilesystemWriteU32(*manifest, entryCursor + 24, entry.nameLength);
    entryCursor += kProfileLogV4FilesystemDirectoryEntrySize;
  }

  size_t extentCursor = extentOffset;
  for (size_t i = 0; i != extents.size(); ++i) {
    const auto& extent = extents[i];
    profileLogV4FilesystemWriteU64(
      *manifest, extentCursor, extentInodes[i]);
    profileLogV4FilesystemWriteU64(
      *manifest, extentCursor + 8, extent.chunk);
    profileLogV4FilesystemWriteU32(
      *manifest, extentCursor + 16, extent.arena);
    profileLogV4FilesystemWriteU32(
      *manifest, extentCursor + 20, extent.payloadSize);
    profileLogV4FilesystemWriteU64(
      *manifest, extentCursor + 24, extent.offset);
    profileLogV4FilesystemWriteU64(
      *manifest, extentCursor + 32, extent.checksum);
    extentCursor += kProfileLogV4FilesystemExtentSize;
  }
  if (!strings.empty()) {
    memcpy(manifest->data() + stringOffset, strings.data(), strings.size());
  }
  return 0;
}

int ProfileLogV4FilesystemBackend::serializeLogicalDeltaLocked(
  const ProfileLogV4FilesystemState& value,
  uint64_t generation,
  const ProfileLogV4ManifestReference& parent,
  const std::vector<uint64_t>& changed,
  const std::vector<uint64_t>& deleted,
  std::vector<uint8_t>* manifest) {
  if (!manifest || !generation || value.generation != generation ||
      value.root != 1 || value.nextInode <= value.root ||
      !parent.generation || parent.generation + 1 != generation ||
      parent.arena >= 2 || parent.arena != (parent.generation & 1) ||
      !value.deltaDepth ||
      value.deltaDepth > kProfileLogV4FilesystemCheckpointInterval - 1) {
    return -EIO;
  }

  struct OutputOperation {
    ProfileLogV4FilesystemDeltaOpcode opcode;
    std::vector<uint8_t> blob;
  };
  std::map<uint64_t, OutputOperation> operations;
  for (uint64_t id : changed) {
    const auto inode = value.inodes.find(id);
    if (!id || id >= value.nextInode || inode == value.inodes.end() ||
        inode->second.id != id ||
        !operations.emplace(
          id, OutputOperation{ProfileLogV4FilesystemDeltaOpcode::Set, {}})
          .second) {
      return -EIO;
    }
    if (int error = profileLogV4FilesystemSerializeDeltaInode(
          inode->second, &operations.at(id).blob)) {
      return error;
    }
  }
  for (uint64_t id : deleted) {
    if (!id || id >= value.nextInode ||
        value.inodes.find(id) != value.inodes.end() ||
        !operations.emplace(
          id, OutputOperation{ProfileLogV4FilesystemDeltaOpcode::Delete, {}})
          .second) {
      return -EIO;
    }
  }
  if (operations.empty()) {
    return -EIO;
  }

  size_t operationBytes = 0;
  size_t blobOffset = 0;
  size_t blobSize = 0;
  size_t total = 0;
  if (!profileLogV4FilesystemCheckedMultiply(
        operations.size(), kProfileLogV4FilesystemDeltaOperationSize,
        &operationBytes) ||
      !profileLogV4FilesystemCheckedAdd(
        kProfileLogV4FilesystemHeaderSize, operationBytes, &blobOffset)) {
    return -EFBIG;
  }
  for (const auto& [_, operation] : operations) {
    if (operation.blob.size() > kProfileLogV4MaxSafeOffset - blobSize) {
      return -EFBIG;
    }
    blobSize += operation.blob.size();
  }
  if (!profileLogV4FilesystemCheckedAdd(blobOffset, blobSize, &total)) {
    return -EFBIG;
  }

  manifest->assign(total, 0);
  std::copy(kProfileLogV4FilesystemDeltaMagic.begin(),
            kProfileLogV4FilesystemDeltaMagic.end(),
            manifest->begin());
  profileLogV4FilesystemWriteU32(
    *manifest, 8, kProfileLogV4FilesystemLogicalSchema2);
  profileLogV4FilesystemWriteU32(
    *manifest, 12, kProfileLogV4FilesystemHeaderSize);
  profileLogV4FilesystemWriteU64(*manifest, 16, generation);
  profileLogV4FilesystemWriteU64(*manifest, 24, value.root);
  profileLogV4FilesystemWriteU64(*manifest, 32, value.nextInode);
  profileLogV4FilesystemWriteU32(
    *manifest, 40, kProfileLogV4FilesystemChunkShift);
  profileLogV4FilesystemWriteU32(*manifest, 44, 0);
  profileLogV4FilesystemWriteU64(*manifest, 48, parent.generation);
  profileLogV4FilesystemWriteU32(*manifest, 56, parent.arena);
  profileLogV4FilesystemWriteU32(*manifest, 60, value.deltaDepth);
  profileLogV4FilesystemWriteU64(*manifest, 64, parent.manifestOffset);
  profileLogV4FilesystemWriteU64(*manifest, 72, parent.manifestSize);
  profileLogV4FilesystemWriteU64(*manifest, 80, parent.manifestRecordChecksum);
  profileLogV4FilesystemWriteU64(
    *manifest, 88, kProfileLogV4FilesystemHeaderSize);
  profileLogV4FilesystemWriteU64(*manifest, 96, operations.size());
  profileLogV4FilesystemWriteU64(*manifest, 104, blobOffset);
  profileLogV4FilesystemWriteU64(*manifest, 112, blobSize);

  size_t operationCursor = kProfileLogV4FilesystemHeaderSize;
  size_t blobCursor = blobOffset;
  for (const auto& [id, operation] : operations) {
    profileLogV4FilesystemWriteU64(*manifest, operationCursor, id);
    profileLogV4FilesystemWriteU32(
      *manifest, operationCursor + 8,
      static_cast<uint32_t>(operation.opcode));
    profileLogV4FilesystemWriteU64(
      *manifest, operationCursor + 16, blobCursor);
    profileLogV4FilesystemWriteU64(
      *manifest, operationCursor + 24, operation.blob.size());
    if (!operation.blob.empty()) {
      memcpy(manifest->data() + blobCursor,
             operation.blob.data(),
             operation.blob.size());
      blobCursor += operation.blob.size();
    }
    operationCursor += kProfileLogV4FilesystemDeltaOperationSize;
  }
  return 0;
}

int ProfileLogV4FilesystemBackend::parseLogicalStateLocked(
  const std::vector<uint8_t>& manifest,
  uint64_t generation,
  ProfileLogV4FilesystemState* result) {
  if (!result || manifest.size() < kProfileLogV4FilesystemHeaderSize ||
      !std::equal(kProfileLogV4FilesystemMagic.begin(),
                  kProfileLogV4FilesystemMagic.end(),
                  manifest.begin())) {
    return -EIO;
  }
  const auto read32 = [&](size_t offset, uint32_t* value) {
    return profileLogV4FilesystemReadU32(
      manifest.data(), manifest.size(), offset, value);
  };
  const auto read64 = [&](size_t offset, uint64_t* value) {
    return profileLogV4FilesystemReadU64(
      manifest.data(), manifest.size(), offset, value);
  };
  uint32_t schema = 0;
  uint32_t headerSize = 0;
  uint64_t encodedGeneration = 0;
  uint64_t root = 0;
  uint64_t nextInode = 0;
  uint32_t chunkShift = 0;
  uint32_t flags = 0;
  uint64_t inodeOffset = 0;
  uint64_t inodeCount64 = 0;
  uint64_t directoryOffset = 0;
  uint64_t directoryCount64 = 0;
  uint64_t extentOffset = 0;
  uint64_t extentCount64 = 0;
  uint64_t stringOffset64 = 0;
  uint64_t stringSize64 = 0;
  uint64_t reserved112 = 0;
  uint64_t reserved120 = 0;
  if (!read32(8, &schema) || !read32(12, &headerSize) ||
      !read64(16, &encodedGeneration) || !read64(24, &root) ||
      !read64(32, &nextInode) || !read32(40, &chunkShift) ||
      !read32(44, &flags) || !read64(48, &inodeOffset) ||
      !read64(56, &inodeCount64) || !read64(64, &directoryOffset) ||
      !read64(72, &directoryCount64) || !read64(80, &extentOffset) ||
      !read64(88, &extentCount64) || !read64(96, &stringOffset64) ||
      !read64(104, &stringSize64) || !read64(112, &reserved112) ||
      !read64(120, &reserved120) ||
      schema != kProfileLogV4FilesystemLogicalSchema1 ||
      headerSize != kProfileLogV4FilesystemHeaderSize ||
      encodedGeneration != generation || !generation || root != 1 ||
      !nextInode || chunkShift != kProfileLogV4FilesystemChunkShift || flags ||
      reserved112 || reserved120 || inodeCount64 >
        std::numeric_limits<size_t>::max() || directoryCount64 >
        std::numeric_limits<size_t>::max() || extentCount64 >
        std::numeric_limits<size_t>::max()) {
    return -EIO;
  }
  const size_t inodeCount = static_cast<size_t>(inodeCount64);
  const size_t directoryCount = static_cast<size_t>(directoryCount64);
  const size_t extentCount = static_cast<size_t>(extentCount64);
  size_t inodeBytes = 0;
  size_t directoryBytes = 0;
  size_t extentBytes = 0;
  size_t expectedDirectoryOffset = 0;
  size_t expectedExtentOffset = 0;
  size_t expectedStringOffset = 0;
  size_t expectedSize = 0;
  if (!inodeCount || !profileLogV4FilesystemCheckedMultiply(
        inodeCount, kProfileLogV4FilesystemInodeSize, &inodeBytes) ||
      !profileLogV4FilesystemCheckedMultiply(
        directoryCount, kProfileLogV4FilesystemDirectoryEntrySize,
        &directoryBytes) ||
      !profileLogV4FilesystemCheckedMultiply(
        extentCount, kProfileLogV4FilesystemExtentSize, &extentBytes) ||
      !profileLogV4FilesystemCheckedAdd(
        kProfileLogV4FilesystemHeaderSize, inodeBytes,
        &expectedDirectoryOffset) ||
      !profileLogV4FilesystemCheckedAdd(
        expectedDirectoryOffset, directoryBytes, &expectedExtentOffset) ||
      !profileLogV4FilesystemCheckedAdd(
        expectedExtentOffset, extentBytes, &expectedStringOffset) ||
      stringSize64 > std::numeric_limits<size_t>::max() ||
      !profileLogV4FilesystemCheckedAdd(
        expectedStringOffset,
        static_cast<size_t>(stringSize64),
        &expectedSize) ||
      inodeOffset != kProfileLogV4FilesystemHeaderSize ||
      directoryOffset != expectedDirectoryOffset ||
      extentOffset != expectedExtentOffset ||
      stringOffset64 != expectedStringOffset || expectedSize != manifest.size()) {
    return -EIO;
  }
  size_t stringOffset = 0;
  size_t stringSize = 0;
  if (!profileLogV4FilesystemRange(
        manifest.size(), stringOffset64, stringSize64, &stringOffset,
        &stringSize) || stringOffset + stringSize != manifest.size()) {
    return -EIO;
  }

  struct InodeAux {
    uint64_t entryFirst;
    uint64_t entryCount;
    uint64_t extentFirst;
    uint64_t extentCount;
    uint64_t targetOffset;
    uint64_t targetLength;
  };
  struct DirectoryEntry {
    uint64_t parent;
    uint64_t child;
    std::string name;
  };
  struct ExtentEntry {
    uint64_t inode;
    ProfileLogV4FilesystemExtent extent;
  };
  ProfileLogV4FilesystemState value;
  value.generation = generation;
  value.root = root;
  value.nextInode = nextInode;
  std::map<uint64_t, InodeAux> inodeAux;
  uint64_t previousInode = 0;
  for (size_t index = 0; index != inodeCount; ++index) {
    const size_t offset = kProfileLogV4FilesystemHeaderSize +
                          index * kProfileLogV4FilesystemInodeSize;
    uint64_t id = 0;
    uint32_t kindRaw = 0;
    uint32_t reserved12 = 0;
    uint32_t mode = 0;
    uint32_t reserved20 = 0;
    uint64_t atime = 0;
    uint64_t mtime = 0;
    uint64_t ctime = 0;
    uint64_t size = 0;
    InodeAux aux = {};
    uint64_t reserved104 = 0;
    if (!read64(offset, &id) || !read32(offset + 8, &kindRaw) ||
        !read32(offset + 12, &reserved12) || !read32(offset + 16, &mode) ||
        !read32(offset + 20, &reserved20) || !read64(offset + 24, &atime) ||
        !read64(offset + 32, &mtime) || !read64(offset + 40, &ctime) ||
        !read64(offset + 48, &size) || !read64(offset + 56, &aux.entryFirst) ||
        !read64(offset + 64, &aux.entryCount) ||
        !read64(offset + 72, &aux.extentFirst) ||
        !read64(offset + 80, &aux.extentCount) ||
        !read64(offset + 88, &aux.targetOffset) ||
        !read64(offset + 96, &aux.targetLength) ||
        !read64(offset + 104, &reserved104) || !id || id <= previousInode ||
        id > static_cast<uint64_t>(std::numeric_limits<ino_t>::max()) ||
        reserved12 || reserved20 || reserved104) {
      return -EIO;
    }
    ProfileLogV4FilesystemInodeKind kind;
    switch (kindRaw) {
      case static_cast<uint32_t>(ProfileLogV4FilesystemInodeKind::Regular):
        kind = ProfileLogV4FilesystemInodeKind::Regular;
        break;
      case static_cast<uint32_t>(ProfileLogV4FilesystemInodeKind::Directory):
        kind = ProfileLogV4FilesystemInodeKind::Directory;
        break;
      case static_cast<uint32_t>(ProfileLogV4FilesystemInodeKind::Symlink):
        kind = ProfileLogV4FilesystemInodeKind::Symlink;
        break;
      default:
        return -EIO;
    }
    File::Metadata metadata = {static_cast<mode_t>(mode),
                               profileLogV4FilesystemDoubleFromBits(atime),
                               profileLogV4FilesystemDoubleFromBits(mtime),
                               profileLogV4FilesystemDoubleFromBits(ctime)};
    if (!profileLogV4FilesystemValidMetadata(metadata, kind) ||
        size > kProfileLogV4MaxSafeOffset) {
      return -EIO;
    }
    ProfileLogV4FilesystemInode inode;
    inode.id = id;
    inode.kind = kind;
    inode.metadata = metadata;
    inode.size = size;
    if (kind == ProfileLogV4FilesystemInodeKind::Symlink) {
      size_t targetOffset = 0;
      size_t targetLength = 0;
      if (!profileLogV4FilesystemRange(
            stringSize, aux.targetOffset, aux.targetLength, &targetOffset,
            &targetLength)) {
        return -EIO;
      }
      inode.target.assign(reinterpret_cast<const char*>(
                            manifest.data() + stringOffset + targetOffset),
                          targetLength);
      if (inode.target.find('\0') != std::string::npos ||
          aux.entryFirst || aux.entryCount || aux.extentFirst ||
          aux.extentCount || size) {
        return -EIO;
      }
    } else if (aux.targetOffset || aux.targetLength ||
               (kind == ProfileLogV4FilesystemInodeKind::Directory &&
                (aux.extentFirst || aux.extentCount || size)) ||
               (kind == ProfileLogV4FilesystemInodeKind::Regular &&
                (aux.entryFirst || aux.entryCount))) {
      return -EIO;
    }
    value.inodes.emplace(id, std::move(inode));
    inodeAux.emplace(id, aux);
    previousInode = id;
  }
  if (value.inodes.find(root) == value.inodes.end() ||
      value.inodes.at(root).kind != ProfileLogV4FilesystemInodeKind::Directory ||
      nextInode <= previousInode) {
    return -EIO;
  }

  std::vector<DirectoryEntry> entries;
  entries.reserve(directoryCount);
  uint64_t previousParent = 0;
  std::string previousName;
  for (size_t index = 0; index != directoryCount; ++index) {
    const size_t offset = expectedDirectoryOffset +
                          index * kProfileLogV4FilesystemDirectoryEntrySize;
    uint64_t parent = 0;
    uint64_t child = 0;
    uint64_t nameOffset = 0;
    uint32_t nameLength = 0;
    uint32_t reserved28 = 0;
    uint64_t reserved32 = 0;
    size_t checkedNameOffset = 0;
    size_t checkedNameLength = 0;
    if (!read64(offset, &parent) || !read64(offset + 8, &child) ||
        !read64(offset + 16, &nameOffset) || !read32(offset + 24, &nameLength) ||
        !read32(offset + 28, &reserved28) || !read64(offset + 32, &reserved32) ||
        reserved28 || reserved32 || !profileLogV4FilesystemRange(
          stringSize, nameOffset, nameLength, &checkedNameOffset,
          &checkedNameLength)) {
      return -EIO;
    }
    std::string name(reinterpret_cast<const char*>(
                       manifest.data() + stringOffset + checkedNameOffset),
                     checkedNameLength);
    if (!profileLogV4FilesystemValidName(name) ||
        (index && (parent < previousParent ||
                   (parent == previousParent && !(previousName < name))))) {
      return -EIO;
    }
    entries.push_back({parent, child, std::move(name)});
    previousParent = parent;
    previousName = entries.back().name;
  }

  std::vector<ExtentEntry> extents;
  extents.reserve(extentCount);
  uint64_t previousExtentInode = 0;
  uint64_t previousChunk = 0;
  for (size_t index = 0; index != extentCount; ++index) {
    const size_t offset = expectedExtentOffset +
                          index * kProfileLogV4FilesystemExtentSize;
    ExtentEntry entry = {};
    uint32_t reserved40 = 0;
    uint64_t reserved40high = 0;
    if (!read64(offset, &entry.inode) ||
        !read64(offset + 8, &entry.extent.chunk) ||
        !read32(offset + 16, &entry.extent.arena) ||
        !read32(offset + 20, &entry.extent.payloadSize) ||
        !read64(offset + 24, &entry.extent.offset) ||
        !read64(offset + 32, &entry.extent.checksum) ||
        !read64(offset + 40, &reserved40high) || reserved40 || reserved40high ||
        entry.extent.arena >= 2 ||
        entry.extent.payloadSize != kProfileLogV4FilesystemChunkSize ||
        (index && (entry.inode < previousExtentInode ||
                   (entry.inode == previousExtentInode &&
                    entry.extent.chunk <= previousChunk)))) {
      return -EIO;
    }
    extents.push_back(entry);
    previousExtentInode = entry.inode;
    previousChunk = entry.extent.chunk;
  }

  std::map<uint64_t, size_t> parentCounts;
  size_t entryCursor = 0;
  for (auto& [id, inode] : value.inodes) {
    const auto& aux = inodeAux.at(id);
    if (inode.kind != ProfileLogV4FilesystemInodeKind::Directory) {
      continue;
    }
    const size_t first = entryCursor;
    while (entryCursor < entries.size() && entries[entryCursor].parent == id) {
      const auto& entry = entries[entryCursor++];
      const auto parent = value.inodes.find(entry.parent);
      const auto child = value.inodes.find(entry.child);
      if (parent == value.inodes.end() || child == value.inodes.end() ||
          parent->second.kind != ProfileLogV4FilesystemInodeKind::Directory ||
          !inode.entries.emplace(entry.name, entry.child).second) {
        return -EIO;
      }
      ++parentCounts[entry.child];
    }
    if (aux.entryFirst != first || aux.entryCount != entryCursor - first) {
      return -EIO;
    }
  }
  if (entryCursor != entries.size()) {
    return -EIO;
  }

  size_t extentCursor = 0;
  for (auto& [id, inode] : value.inodes) {
    const auto& aux = inodeAux.at(id);
    if (inode.kind != ProfileLogV4FilesystemInodeKind::Regular) {
      continue;
    }
    const size_t first = extentCursor;
    while (extentCursor < extents.size() && extents[extentCursor].inode == id) {
      const auto& entry = extents[extentCursor++];
      if (entry.extent.chunk >
            (inode.size ? (inode.size - 1) >> kProfileLogV4FilesystemChunkShift
                        : 0) ||
          !inode.extents.emplace(entry.extent.chunk, entry.extent).second) {
        return -EIO;
      }
    }
    if (aux.extentFirst != first || aux.extentCount != extentCursor - first) {
      return -EIO;
    }
  }
  if (extentCursor != extents.size()) {
    return -EIO;
  }
  for (const auto& [id, _] : value.inodes) {
    if (id == root) {
      if (parentCounts[id]) {
        return -EIO;
      }
      continue;
    }
    if (parentCounts[id] != 1) {
      return -EIO;
    }
  }
  for (const auto& [id, _] : value.inodes) {
    std::unordered_set<uint64_t> seen;
    uint64_t current = id;
    while (current != root) {
      if (!seen.insert(current).second) {
        return -EIO;
      }
      bool found = false;
      for (const auto& [parentID, parent] : value.inodes) {
        if (parent.kind != ProfileLogV4FilesystemInodeKind::Directory) {
          continue;
        }
        for (const auto& [_, child] : parent.entries) {
          if (child == current) {
            current = parentID;
            found = true;
            break;
          }
        }
        if (found) {
          break;
        }
      }
      if (!found) {
        return -EIO;
      }
    }
  }
  // A Schema-1 payload can be an ancestor of a selected Schema-2 delta.
  // Its structural namespace is checked here; the loader later validates
  // each reachable snapshot's extents against that snapshot's generation
  // while retaining the selected descriptor's high-water bounds.
  if (int error = validateLogicalStateLocked(value, generation, false)) {
    return error;
  }
  *result = std::move(value);
  return 0;
}

int ProfileLogV4FilesystemBackend::parseLogicalDeltaLocked(
  const std::vector<uint8_t>& manifest,
  uint64_t generation,
  ProfileLogV4FilesystemDelta* result) {
  if (!result || manifest.size() < kProfileLogV4FilesystemHeaderSize ||
      !std::equal(kProfileLogV4FilesystemDeltaMagic.begin(),
                  kProfileLogV4FilesystemDeltaMagic.end(),
                  manifest.begin())) {
    return -EIO;
  }
  const auto read32 = [&](size_t offset, uint32_t* value) {
    return profileLogV4FilesystemReadU32(
      manifest.data(), manifest.size(), offset, value);
  };
  const auto read64 = [&](size_t offset, uint64_t* value) {
    return profileLogV4FilesystemReadU64(
      manifest.data(), manifest.size(), offset, value);
  };
  uint32_t schema = 0;
  uint32_t headerSize = 0;
  uint64_t encodedGeneration = 0;
  uint64_t root = 0;
  uint64_t nextInode = 0;
  uint32_t chunkShift = 0;
  uint32_t flags = 0;
  uint64_t parentGeneration = 0;
  uint32_t parentArena = 0;
  uint32_t depth = 0;
  uint64_t parentManifestOffset = 0;
  uint64_t parentManifestSize = 0;
  uint64_t parentManifestRecordChecksum = 0;
  uint64_t operationOffset = 0;
  uint64_t operationCount64 = 0;
  uint64_t blobOffset64 = 0;
  uint64_t blobSize64 = 0;
  uint64_t reserved120 = 0;
  if (!read32(8, &schema) || !read32(12, &headerSize) ||
      !read64(16, &encodedGeneration) || !read64(24, &root) ||
      !read64(32, &nextInode) || !read32(40, &chunkShift) ||
      !read32(44, &flags) || !read64(48, &parentGeneration) ||
      !read32(56, &parentArena) || !read32(60, &depth) ||
      !read64(64, &parentManifestOffset) ||
      !read64(72, &parentManifestSize) ||
      !read64(80, &parentManifestRecordChecksum) ||
      !read64(88, &operationOffset) || !read64(96, &operationCount64) ||
      !read64(104, &blobOffset64) || !read64(112, &blobSize64) ||
      !read64(120, &reserved120) ||
      schema != kProfileLogV4FilesystemLogicalSchema2 ||
      headerSize != kProfileLogV4FilesystemHeaderSize ||
      encodedGeneration != generation || !generation || root != 1 ||
      nextInode <= root ||
      chunkShift != kProfileLogV4FilesystemChunkShift || flags ||
      !parentGeneration || parentGeneration + 1 != generation ||
      parentArena >= 2 || parentArena != (parentGeneration & 1) || !depth ||
      depth > kProfileLogV4FilesystemCheckpointInterval - 1 ||
      parentManifestOffset >
        kProfileLogV4MaxSafeOffset - kProfileLogV4ManifestHeaderSize ||
      parentManifestSize > kProfileLogV4MaxSafeOffset -
        parentManifestOffset - kProfileLogV4ManifestHeaderSize ||
      reserved120 || !operationCount64 ||
      operationCount64 > std::numeric_limits<size_t>::max()) {
    return -EIO;
  }
  const size_t operationCount = static_cast<size_t>(operationCount64);
  size_t operationBytes = 0;
  size_t expectedBlobOffset = 0;
  if (!profileLogV4FilesystemCheckedMultiply(
        operationCount, kProfileLogV4FilesystemDeltaOperationSize,
        &operationBytes) ||
      !profileLogV4FilesystemCheckedAdd(
        kProfileLogV4FilesystemHeaderSize, operationBytes,
        &expectedBlobOffset) ||
      operationOffset != kProfileLogV4FilesystemHeaderSize ||
      blobOffset64 != expectedBlobOffset ||
      expectedBlobOffset > manifest.size() ||
      blobSize64 != manifest.size() - expectedBlobOffset) {
    return -EIO;
  }

  ProfileLogV4FilesystemDelta delta;
  delta.generation = generation;
  delta.root = root;
  delta.nextInode = nextInode;
  delta.depth = depth;
  delta.parent = {parentGeneration,
                  parentArena,
                  parentManifestOffset,
                  parentManifestSize,
                  parentManifestRecordChecksum};
  size_t blobCursor = expectedBlobOffset;
  uint64_t previousInode = 0;
  for (size_t index = 0; index != operationCount; ++index) {
    const size_t offset = kProfileLogV4FilesystemHeaderSize +
                          index * kProfileLogV4FilesystemDeltaOperationSize;
    uint64_t inode = 0;
    uint32_t opcodeRaw = 0;
    uint32_t reserved12 = 0;
    uint64_t blobOffset = 0;
    uint64_t blobSize = 0;
    if (!read64(offset, &inode) || !read32(offset + 8, &opcodeRaw) ||
        !read32(offset + 12, &reserved12) || !read64(offset + 16, &blobOffset) ||
        !read64(offset + 24, &blobSize) || !inode ||
        inode <= previousInode || reserved12 || blobOffset != blobCursor) {
      return -EIO;
    }
    ProfileLogV4FilesystemDeltaOperation operation;
    operation.inode = inode;
    switch (opcodeRaw) {
      case static_cast<uint32_t>(ProfileLogV4FilesystemDeltaOpcode::Set): {
        if (blobSize < kProfileLogV4FilesystemDeltaInodeHeaderSize ||
            blobSize > manifest.size() - blobCursor) {
          return -EIO;
        }
        operation.opcode = ProfileLogV4FilesystemDeltaOpcode::Set;
        if (int error = profileLogV4FilesystemParseDeltaInode(
              manifest.data() + blobCursor,
              static_cast<size_t>(blobSize),
              inode,
              &operation.value)) {
          return error;
        }
        blobCursor += static_cast<size_t>(blobSize);
        break;
      }
      case static_cast<uint32_t>(ProfileLogV4FilesystemDeltaOpcode::Delete):
        if (blobSize) {
          return -EIO;
        }
        operation.opcode = ProfileLogV4FilesystemDeltaOpcode::Delete;
        break;
      default:
        return -EIO;
    }
    delta.operations.push_back(std::move(operation));
    previousInode = inode;
  }
  if (blobCursor != manifest.size()) {
    return -EIO;
  }
  *result = std::move(delta);
  return 0;
}

int ProfileLogV4FilesystemBackend::readExtentLocked(
  const ProfileLogV4FilesystemExtent& extent,
  uint64_t inode,
  uint64_t chunk,
  uint64_t selectedGeneration,
  std::vector<uint8_t>* bytes,
  bool selectedAlreadyValidated,
  uint64_t maxRecordGeneration) {
  if (!bytes || !inode || extent.arena >= 2 ||
      extent.payloadSize != kProfileLogV4FilesystemChunkSize ||
      extent.offset > kProfileLogV4MaxSafeOffset -
                        kProfileLogV4FilesystemDataHeaderSize ||
      extent.payloadSize > kProfileLogV4MaxSafeOffset - extent.offset -
                             kProfileLogV4FilesystemDataHeaderSize) {
    return -EIO;
  }
  if (!maxRecordGeneration) {
    maxRecordGeneration = selectedGeneration;
  }
  if (!selectedGeneration || !maxRecordGeneration) {
    return -EIO;
  }
  std::array<uint8_t, kProfileLogV4FilesystemDataHeaderSize> header;
  const auto readSelected = [&](uint32_t arena,
                                uint64_t offset,
                                uint8_t* destination,
                                size_t size) {
    return selectedAlreadyValidated
             ? readSelectedBytesDuringValidatedTransaction(
                 arena, offset, destination, size)
             : readSelectedBytesFromValidatedSnapshot(
                 selectedGeneration, arena, offset, destination, size);
  };
  if (int error = readSelected(
        extent.arena, extent.offset, header.data(), header.size())) {
    return error;
  }
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_EXTENT_CORRUPTION
  // The selected outer descriptor still authenticates this byte range.  For
  // the chronology test, alter only the stack-local historical data header
  // into a syntactically valid record from the selected future generation.
  // A normal mount can then prove that no OPFS byte or selected descriptor
  // was changed by the failed replay.
  if (!selectedAlreadyValidated && maxRecordGeneration < selectedGeneration) {
    writeProfileLogV4U64(header, 16, selectedGeneration);
    writeProfileLogV4U64(
      header, 56, profileLogV4ChecksumWithZeroedRange(header, 56, 8));
  }
#endif
  uint64_t recordGeneration = 0;
  uint64_t recordInode = 0;
  uint64_t recordChunk = 0;
  uint32_t payloadSize = 0;
  uint64_t payloadChecksum = 0;
  if (!profileLogV4FilesystemParseDataHeader(header,
                                              &recordGeneration,
                                              &recordInode,
                                              &recordChunk,
                                              &payloadSize,
                                              &payloadChecksum) ||
      recordGeneration > maxRecordGeneration ||
      static_cast<uint32_t>(recordGeneration & 1) != extent.arena ||
      recordInode != inode || recordChunk != chunk ||
      payloadSize != extent.payloadSize || payloadChecksum != extent.checksum) {
    return -EIO;
  }
  bytes->resize(payloadSize);
  if (int error = readSelected(extent.arena,
                               extent.offset + header.size(),
                               bytes->data(),
                               bytes->size())) {
    return error;
  }
  return profileLogV4Checksum(bytes->data(), bytes->size()) == payloadChecksum
           ? 0
           : -EIO;
}

int ProfileLogV4FilesystemBackend::readChunkLocked(
  const ProfileLogV4FilesystemInode& inode,
  uint64_t chunk,
  uint64_t generation,
  std::vector<uint8_t>* bytes) {
  if (!bytes || inode.kind != ProfileLogV4FilesystemInodeKind::Regular) {
    return -EINVAL;
  }
  if (auto volatileChunk = inode.volatileChunks.find(chunk);
      volatileChunk != inode.volatileChunks.end()) {
    if (volatileChunk->second.size() != kProfileLogV4FilesystemChunkSize) {
      return -EIO;
    }
    *bytes = volatileChunk->second;
    return 0;
  }
  if (auto extent = inode.extents.find(chunk); extent != inode.extents.end()) {
    return readExtentLocked(extent->second, inode.id, chunk, generation, bytes);
  }
  bytes->assign(kProfileLogV4FilesystemChunkSize, 0);
  return 0;
}

int ProfileLogV4FilesystemBackend::getChunkForWriteLocked(
  const ProfileLogV4FilesystemInode& inode,
  uint64_t chunk,
  std::vector<uint8_t>* bytes) {
  return readChunkLocked(inode, chunk, state.generation, bytes);
}

int ProfileLogV4FilesystemBackend::appendImmutableChunkLocked(
  Transaction& transaction,
  uint64_t inode,
  uint64_t chunk,
  const std::vector<uint8_t>& bytes,
  ProfileLogV4FilesystemExtent* result) {
  if (!result || !inode ||
      bytes.size() != kProfileLogV4FilesystemChunkSize) {
    return -EIO;
  }
  const auto header = profileLogV4FilesystemMakeDataHeader(
    transaction.generation(), inode, chunk, bytes.data(), bytes.size());
  std::vector<uint8_t> record;
  record.reserve(header.size() + bytes.size());
  record.insert(record.end(), header.begin(), header.end());
  record.insert(record.end(), bytes.begin(), bytes.end());
  uint64_t offset = 0;
  if (int error = transaction.append(record.data(), record.size(), &offset)) {
    return error;
  }
  *result = {chunk,
             transaction.arena(),
             static_cast<uint32_t>(bytes.size()),
             offset,
             profileLogV4Checksum(bytes.data(), bytes.size())};
  return 0;
}

int ProfileLogV4FilesystemBackend::copyLiveCheckpointExtentsLocked(
  ProfileLogV4FilesystemState* value,
  Transaction& transaction) {
  if (!value || !state.generation) {
    return -EIO;
  }
  for (auto& [inodeID, inode] : value->inodes) {
    if (inode.kind != ProfileLogV4FilesystemInodeKind::Regular) {
      continue;
    }
    for (auto& [chunk, extent] : inode.extents) {
      std::vector<uint8_t> bytes;
      // The selected pre-image remains authoritative until the outer V4
      // descriptor has both phase witnesses. Never read a just-appended
      // checkpoint record through readSelectedBytes() here.
      if (int error = readExtentLocked(
            extent, inodeID, chunk, state.generation, &bytes, true)) {
        return error;
      }
      ProfileLogV4FilesystemExtent copied;
      if (int error = appendImmutableChunkLocked(
            transaction, inodeID, chunk, bytes, &copied)) {
        return error;
      }
      extent = copied;
    }
  }
  return 0;
}

int ProfileLogV4FilesystemBackend::validateLogicalStateLocked(
  const ProfileLogV4FilesystemState& value,
  uint64_t generation,
  bool validateExtents,
  uint64_t selectedGeneration) {
  if (!generation || value.generation != generation || value.root != 1 ||
      value.nextInode <= value.root || value.inodes.empty() ||
      value.inodes.find(value.root) == value.inodes.end() ||
      value.inodes.at(value.root).kind !=
        ProfileLogV4FilesystemInodeKind::Directory) {
    return -EIO;
  }
  if (!selectedGeneration) {
    selectedGeneration = generation;
  }
  if (!selectedGeneration || selectedGeneration < generation) {
    return -EIO;
  }
  std::map<uint64_t, size_t> parents;
  uint64_t previousID = 0;
  for (const auto& [id, inode] : value.inodes) {
    if (!id || id <= previousID ||
        id > static_cast<uint64_t>(std::numeric_limits<ino_t>::max()) ||
        id >= value.nextInode || inode.id != id ||
        !profileLogV4FilesystemValidMetadata(inode.metadata, inode.kind)) {
      return -EIO;
    }
    if (inode.kind == ProfileLogV4FilesystemInodeKind::Directory) {
      if (inode.size || !inode.extents.empty() || !inode.target.empty() ||
          !inode.volatileChunks.empty()) {
        return -EIO;
      }
      for (const auto& [name, child] : inode.entries) {
        if (!profileLogV4FilesystemValidName(name) ||
            value.inodes.find(child) == value.inodes.end() ||
            ++parents[child] > 1) {
          return -EIO;
        }
      }
    } else if (inode.kind == ProfileLogV4FilesystemInodeKind::Regular) {
      if (!inode.entries.empty() || !inode.target.empty() ||
          inode.size > kProfileLogV4MaxSafeOffset ||
          !inode.volatileChunks.empty()) {
        return -EIO;
      }
      uint64_t previousChunk = 0;
      bool haveChunk = false;
      for (const auto& [chunk, extent] : inode.extents) {
        if ((haveChunk && chunk <= previousChunk) || extent.chunk != chunk ||
            extent.arena >= 2 ||
            extent.payloadSize != kProfileLogV4FilesystemChunkSize ||
            !inode.size ||
            chunk > ((inode.size - 1) >> kProfileLogV4FilesystemChunkShift)) {
          return -EIO;
        }
        if (validateExtents) {
          std::vector<uint8_t> bytes;
          if (int error = readExtentLocked(extent,
                                           id,
                                           chunk,
                                           selectedGeneration,
                                           &bytes,
                                           false,
                                           generation)) {
            return error;
          }
        }
        previousChunk = chunk;
        haveChunk = true;
      }
    } else if (inode.kind == ProfileLogV4FilesystemInodeKind::Symlink) {
      if (inode.size || !inode.entries.empty() || !inode.extents.empty() ||
          !inode.volatileChunks.empty() ||
          inode.target.find('\0') != std::string::npos) {
        return -EIO;
      }
    } else {
      return -EIO;
    }
    previousID = id;
  }
  for (const auto& [id, _] : value.inodes) {
    if (id == value.root) {
      if (parents[id]) {
        return -EIO;
      }
    } else if (parents[id] != 1) {
      return -EIO;
    }
  }
  for (const auto& [id, _] : value.inodes) {
    std::unordered_set<uint64_t> seen;
    uint64_t current = id;
    while (current != value.root) {
      if (!seen.insert(current).second) {
        return -EIO;
      }
      bool found = false;
      for (const auto& [parentID, parent] : value.inodes) {
        if (parent.kind != ProfileLogV4FilesystemInodeKind::Directory) {
          continue;
        }
        for (const auto& [_, child] : parent.entries) {
          if (child == current) {
            current = parentID;
            found = true;
            break;
          }
        }
        if (found) {
          break;
        }
      }
      if (!found) {
        return -EIO;
      }
    }
  }
  return 0;
}

int ProfileLogV4FilesystemBackend::loadLogicalStateLocked() {
  size_t size = 0;
  uint64_t generation = 0;
  if (int error = readManifest(nullptr, 0, &size, &generation)) {
    return error;
  }
  if (!generation) {
    return -EIO;
  }
  // The only root-less filesystem state is the unexposed g=1 bootstrap.
  // A selected empty manifest at any later generation must never be treated
  // as a fresh root: doing so would let corruption turn a reload into a
  // successful replacement of an established profile tree.
  if ((generation == 1) != (size == 0)) {
    return -EIO;
  }
  if (!size) {
    state = {};
    state.generation = generation;
    state.nextInode = 1;
    logicalStateLoaded = true;
    return 0;
  }
  std::vector<uint8_t> manifest(size);
  uint64_t payloadGeneration = 0;
  if (int error = readManifest(
        manifest.data(), manifest.size(), &size, &payloadGeneration)) {
    return error;
  }
  if (size != manifest.size() || payloadGeneration != generation) {
    return -EIO;
  }
  std::vector<ProfileLogV4FilesystemDelta> deltas;
  std::vector<uint8_t> current = std::move(manifest);
  uint64_t currentGeneration = generation;
  std::optional<uint32_t> expectedParentDepth;
  ProfileLogV4FilesystemState loaded;
  for (;;) {
    if (current.size() >= kProfileLogV4FilesystemMagic.size() &&
        std::equal(kProfileLogV4FilesystemMagic.begin(),
                   kProfileLogV4FilesystemMagic.end(), current.begin())) {
      if (int error = parseLogicalStateLocked(
            current, currentGeneration, &loaded)) {
        return error;
      }
      break;
    }
    if (current.size() < kProfileLogV4FilesystemDeltaMagic.size() ||
        !std::equal(kProfileLogV4FilesystemDeltaMagic.begin(),
                    kProfileLogV4FilesystemDeltaMagic.end(),
                    current.begin())) {
      return -EIO;
    }
    ProfileLogV4FilesystemDelta delta;
    if (int error = parseLogicalDeltaLocked(
          current, currentGeneration, &delta)) {
      return error;
    }
    if ((expectedParentDepth && delta.depth != *expectedParentDepth) ||
        delta.depth > kProfileLogV4FilesystemCheckpointInterval - 1 ||
        deltas.size() >= kProfileLogV4FilesystemCheckpointInterval - 1) {
      return -EIO;
    }
    expectedParentDepth = delta.depth - 1;
    deltas.push_back(std::move(delta));
    const auto& parent = deltas.back().parent;
    if (int error = readSelectedHistoricalManifest(parent, &current)) {
      return error;
    }
    currentGeneration = parent.generation;
  }
  if ((expectedParentDepth && *expectedParentDepth != 0) ||
      (!deltas.empty() && deltas.front().depth != deltas.size())) {
    return -EIO;
  }
  // A historical checkpoint is still an authenticated filesystem state in
  // its own right.  Validate its data records before a later delta can
  // replace or delete a bad extent and hide corruption in its ancestor.
  // The selected descriptor admits the byte ranges; currentGeneration sets
  // the upper bound for immutable data-record generations in this snapshot.
  if (!deltas.empty()) {
    if (int error = validateLogicalStateLocked(
          loaded, currentGeneration, true, generation)) {
      return error;
    }
  }
  loaded.deltaDepth = 0;
  for (auto delta = deltas.rbegin(); delta != deltas.rend(); ++delta) {
    if (delta->parent.generation != loaded.generation ||
        delta->depth != loaded.deltaDepth + 1 ||
        delta->root != loaded.root ||
        delta->nextInode < loaded.nextInode) {
      return -EIO;
    }
    for (const auto& operation : delta->operations) {
      if (operation.opcode == ProfileLogV4FilesystemDeltaOpcode::Set) {
        loaded.inodes[operation.inode] = operation.value;
      } else if (operation.opcode == ProfileLogV4FilesystemDeltaOpcode::Delete) {
        if (loaded.inodes.erase(operation.inode) != 1) {
          return -EIO;
        }
      } else {
        return -EIO;
      }
    }
    loaded.generation = delta->generation;
    loaded.nextInode = delta->nextInode;
    loaded.deltaDepth = delta->depth;
    // Every ancestor was once a selected filesystem state. Reject a chain
    // that reaches a valid final tree only by passing through an invalid
    // intermediate namespace or extent, rather than accepting it merely
    // because a later delta happens to repair the damage.  The selected
    // descriptor admits the historical bytes, while this state generation
    // prevents a future data record from validating an older snapshot.
    if (int error = validateLogicalStateLocked(
          loaded, delta->generation, true, generation)) {
      return error;
    }
  }
  if (loaded.generation != generation ||
      (deltas.empty() && loaded.deltaDepth) ||
      (!deltas.empty() && loaded.deltaDepth != deltas.front().depth) ||
      validateLogicalStateLocked(loaded, generation, true)) {
    return -EIO;
  }
  state = std::move(loaded);
  logicalStateLoaded = true;
  return 0;
}

int ProfileLogV4FilesystemBackend::flushLocked() {
  if (int error = validateCurrentStateLocked()) {
    return error;
  }
  // A drain is a durability boundary, not a hot path. Reauthenticate the
  // complete selected payload before releasing the profile lease.
  size_t manifestSize = 0;
  uint64_t manifestGeneration = 0;
  if (int error = readManifest(
        nullptr, 0, &manifestSize, &manifestGeneration)) {
    return poisonFilesystemLocked(error);
  }
  if (manifestGeneration != state.generation ||
      ((manifestGeneration == 1) != (manifestSize == 0))) {
    return poisonFilesystemLocked(-EIO);
  }
  if (!state.root) {
    return state.nextInode == 1 && state.inodes.empty() ? 0
                                                         : poisonFilesystemLocked(-EIO);
  }
  if (int error = validateLogicalStateLocked(
        state, state.generation, true)) {
    return poisonFilesystemLocked(error);
  }
  return 0;
}

int ProfileLogV4FilesystemBackend::commitMetadataLocked(
  uint64_t id,
  const File::Metadata& metadata) {
  if (int error = validateCurrentStateLocked()) {
    return error;
  }
  if (auto orphan = orphans.find(id); orphan != orphans.end()) {
    if (!profileLogV4FilesystemValidMetadata(metadata, orphan->second.kind)) {
      return -EINVAL;
    }
    orphan->second.metadata = metadata;
    return 0;
  }
  const auto found = state.inodes.find(id);
  if (found == state.inodes.end() ||
      !profileLogV4FilesystemValidMetadata(metadata, found->second.kind)) {
    return -ENOENT;
  }
  found->second.metadata = metadata;
  return commitLogicalStateLocked({id}, {}, {});
}

int ProfileLogV4FilesystemBackend::commitLogicalStateLocked(
  const std::vector<uint64_t>& changed,
  const std::vector<uint64_t>& deleted,
  std::vector<PendingChunk> pending,
  bool allowReclamation,
  bool forceCheckpoint) {
  if (!state.generation ||
      state.generation == std::numeric_limits<uint64_t>::max()) {
    return poisonFilesystemLocked(-EIO);
  }
  // An unlinked-but-open inode is intentionally absent from |state|. Do not
  // retire either arena while such a live descriptor could still read an old
  // extent. A scheduled or depth-forced checkpoint must still be written as
  // a parent-free Schema-1 snapshot in that case; only physical retirement is
  // deferred until there are no old open descriptors.
  const uint64_t nextGeneration = state.generation + 1;
  const bool checkpoint =
    forceCheckpoint ||
    state.deltaDepth >= kProfileLogV4FilesystemCheckpointInterval - 1 ||
    nextGeneration % kProfileLogV4FilesystemCheckpointInterval == 0;
  const bool reclaim = checkpoint && allowReclamation && orphans.empty();
  std::set<std::pair<uint64_t, uint64_t>> writtenChunks;
  const int result = commitTransaction(
    [&](Transaction& transaction, std::vector<uint8_t>* manifest) {
      if (transaction.generation() != nextGeneration) {
        return -EIO;
      }
      if (reclaim) {
        // Copy durable extents first. Pending writes below are new records in
        // the target arena and cannot be read through the old selected
        // descriptor until this transaction's witness quorum is complete.
        if (int error = copyLiveCheckpointExtentsLocked(&state, transaction)) {
          return error;
        }
      }
      for (const auto& pendingChunk : pending) {
        if (!pendingChunk.inode ||
            pendingChunk.bytes.size() != kProfileLogV4FilesystemChunkSize ||
            !writtenChunks.emplace(pendingChunk.inode, pendingChunk.chunk).second) {
          return -EIO;
        }
        auto inode = state.inodes.find(pendingChunk.inode);
        if (inode == state.inodes.end() ||
            inode->second.kind != ProfileLogV4FilesystemInodeKind::Regular ||
            !inode->second.size ||
            pendingChunk.chunk >
              ((inode->second.size - 1) >> kProfileLogV4FilesystemChunkShift)) {
          return -EIO;
        }
        ProfileLogV4FilesystemExtent written;
        if (int error = appendImmutableChunkLocked(
              transaction,
              pendingChunk.inode,
              pendingChunk.chunk,
              pendingChunk.bytes,
              &written)) {
          return error;
        }
        inode->second.extents[pendingChunk.chunk] = written;
      }
      state.generation = transaction.generation();
      state.deltaDepth = checkpoint ? 0 : state.deltaDepth + 1;
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST
      // Controlled selected-record fault for the reload guard below. It
      // publishes an otherwise valid outer g=2 envelope with an empty logical
      // payload; no production path can serialize that state.
      if (transaction.generation() == 2) {
        manifest->clear();
        return 0;
      }
#endif
      if (checkpoint) {
        if (int error = validateLogicalStateLocked(
              state, transaction.generation(), false)) {
          return error;
        }
        if (reclaim) {
          for (const auto& [_, inode] : state.inodes) {
          if (inode.kind != ProfileLogV4FilesystemInodeKind::Regular) {
            continue;
          }
          for (const auto& [_, extent] : inode.extents) {
            if (extent.arena != transaction.arena()) {
              return -EIO;
            }
          }
        }
          if (int error = transaction.discardInactiveArenaAfterPublish()) {
            return error;
          }
        }
        return serializeLogicalStateLocked(
          state, transaction.generation(), manifest);
      }
      return serializeLogicalDeltaLocked(state,
                                         transaction.generation(),
                                         transaction.parentReference(),
                                         changed,
                                         deleted,
                                         manifest);
    });
  if (result) {
    return poisonFilesystemLocked(result);
  }
  if (reclaim) {
    // commitTransaction() has already published the self-contained outer
    // descriptor. Keep the in-memory tree synchronized with that selected
    // generation before attempting post-quorum physical cleanup; a cleanup
    // failure then latches the backend instead of reviving an old tree.
    if (int error = trimSelectedUnreachableArenaTails()) {
      return poisonFilesystemLocked(error);
    }
  }
  return 0;
}

ssize_t ProfileLogV4FilesystemBackend::writeFileLocked(
  uint64_t id,
  const uint8_t* buffer,
  size_t length,
  off_t offset,
  const File::Metadata& metadata) {
  if (int error = validateCurrentStateLocked()) {
    return error;
  }
  if (offset < 0 || (!buffer && length)) {
    return -EINVAL;
  }
  if (!length) {
    return 0;
  }
  if (length > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
    return -EFBIG;
  }
  const uint64_t start = static_cast<uint64_t>(offset);
  if (start > kProfileLogV4MaxSafeOffset ||
      length > kProfileLogV4MaxSafeOffset - start) {
    return -EFBIG;
  }
  const uint64_t end = start + length;
  auto writeChunks = [&](ProfileLogV4FilesystemInode& inode,
                         std::vector<PendingChunk>* pending,
                         bool durable) -> int {
    size_t cursor = 0;
    while (cursor != length) {
      const uint64_t position = start + cursor;
      const uint64_t chunk = position >> kProfileLogV4FilesystemChunkShift;
      const size_t within = static_cast<size_t>(
        position & (kProfileLogV4FilesystemChunkSize - 1));
      const size_t bytes = std::min(
        kProfileLogV4FilesystemChunkSize - within, length - cursor);
      std::vector<uint8_t> chunkBytes;
      if (int error = getChunkForWriteLocked(inode, chunk, &chunkBytes)) {
        return error;
      }
      memcpy(chunkBytes.data() + within, buffer + cursor, bytes);
      if (durable) {
        inode.extents.erase(chunk);
        pending->push_back({id, chunk, std::move(chunkBytes)});
      } else {
        inode.volatileChunks[chunk] = std::move(chunkBytes);
        inode.extents.erase(chunk);
      }
      cursor += bytes;
    }
    inode.size = std::max(inode.size, end);
    inode.metadata = metadata;
    return 0;
  };
  if (auto orphan = orphans.find(id); orphan != orphans.end()) {
    if (orphan->second.kind != ProfileLogV4FilesystemInodeKind::Regular ||
        !profileLogV4FilesystemValidMetadata(metadata, orphan->second.kind)) {
      return -ENOENT;
    }
    if (int error = writeChunks(orphan->second, nullptr, false)) {
      return poisonFilesystemLocked(error);
    }
    return static_cast<ssize_t>(length);
  }
  const auto found = state.inodes.find(id);
  if (found == state.inodes.end() ||
      found->second.kind != ProfileLogV4FilesystemInodeKind::Regular ||
      !profileLogV4FilesystemValidMetadata(metadata, found->second.kind)) {
    return -ENOENT;
  }
  std::vector<PendingChunk> pending;
  if (int error = writeChunks(state.inodes.at(id), &pending, true)) {
    return poisonFilesystemLocked(error);
  }
  if (int error = commitLogicalStateLocked(
        {id}, {}, std::move(pending))) {
    return error;
  }
  return static_cast<ssize_t>(length);
}

int ProfileLogV4FilesystemBackend::resizeFileLocked(
  uint64_t id,
  off_t size,
  const File::Metadata& metadata) {
  if (int error = validateCurrentStateLocked()) {
    return error;
  }
  if (size < 0 || static_cast<uint64_t>(size) > kProfileLogV4MaxSafeOffset) {
    return -EINVAL;
  }
  const uint64_t newSize = static_cast<uint64_t>(size);
  const auto resize = [&](ProfileLogV4FilesystemInode& inode,
                          std::vector<PendingChunk>* pending,
                          bool durable) -> int {
    if (inode.kind != ProfileLogV4FilesystemInodeKind::Regular ||
        !profileLogV4FilesystemValidMetadata(metadata, inode.kind)) {
      return -ENOENT;
    }
    if (newSize < inode.size) {
      if (!newSize) {
        inode.extents.clear();
        inode.volatileChunks.clear();
      } else {
        const uint64_t lastChunk =
          (newSize - 1) >> kProfileLogV4FilesystemChunkShift;
        for (auto it = inode.extents.begin(); it != inode.extents.end();) {
          if (it->first > lastChunk) {
            it = inode.extents.erase(it);
          } else {
            ++it;
          }
        }
        for (auto it = inode.volatileChunks.begin();
             it != inode.volatileChunks.end();) {
          if (it->first > lastChunk) {
            it = inode.volatileChunks.erase(it);
          } else {
            ++it;
          }
        }
        const size_t tail = static_cast<size_t>(
          newSize & (kProfileLogV4FilesystemChunkSize - 1));
        if (tail) {
          std::vector<uint8_t> chunkBytes;
          if (int error = getChunkForWriteLocked(inode, lastChunk, &chunkBytes)) {
            return error;
          }
          std::fill(chunkBytes.begin() + tail, chunkBytes.end(), 0);
          if (durable) {
            inode.extents.erase(lastChunk);
            pending->push_back({id, lastChunk, std::move(chunkBytes)});
          } else {
            inode.extents.erase(lastChunk);
            inode.volatileChunks[lastChunk] = std::move(chunkBytes);
          }
        }
      }
    }
    inode.size = newSize;
    inode.metadata = metadata;
    return 0;
  };
  if (auto orphan = orphans.find(id); orphan != orphans.end()) {
    if (int error = resize(orphan->second, nullptr, false)) {
      return error;
    }
    return 0;
  }
  if (state.inodes.find(id) == state.inodes.end()) {
    return -ENOENT;
  }
  std::vector<PendingChunk> pending;
  if (int error = resize(state.inodes.at(id), &pending, true)) {
    return poisonFilesystemLocked(error);
  }
  return commitLogicalStateLocked({id}, {}, std::move(pending));
}

int ProfileLogV4FilesystemBackend::commitNamespaceMutationLocked(
  const Directory::NamespaceMutation& mutation) {
  if (int error = validateCurrentStateLocked()) {
    return error;
  }
  const auto directoryID = [&](const std::shared_ptr<Directory>& directory) {
    if (!directory || directory->getBackend() != this ||
        !directory->is<ProfileLogV4FilesystemDirectory>()) {
      return uint64_t(0);
    }
    return std::static_pointer_cast<ProfileLogV4FilesystemDirectory>(directory)
      ->getInode();
  };
  const auto inodeID = [&](const std::shared_ptr<File>& file) {
    if (!file || file->getBackend() != this) {
      return uint64_t(0);
    }
    if (file->is<ProfileLogV4FilesystemDataFile>()) {
      return std::static_pointer_cast<ProfileLogV4FilesystemDataFile>(file)
        ->getInode();
    }
    if (file->is<ProfileLogV4FilesystemDirectory>()) {
      return std::static_pointer_cast<ProfileLogV4FilesystemDirectory>(file)
        ->getInode();
    }
    if (file->is<ProfileLogV4FilesystemSymlink>()) {
      return std::static_pointer_cast<ProfileLogV4FilesystemSymlink>(file)
        ->getInode();
    }
    return uint64_t(0);
  };
  const auto validPostImage = [&](const std::optional<File::Metadata>& image,
                                  ProfileLogV4FilesystemInodeKind kind) {
    return image && profileLogV4FilesystemValidMetadata(*image, kind);
  };
  const auto sourceID = directoryID(mutation.sourceParent);
  const auto destinationID = directoryID(mutation.destinationParent);
  const auto subjectID = inodeID(mutation.subject);
  const auto replacementID = inodeID(mutation.replacement);

  switch (mutation.kind) {
    case Directory::NamespaceMutation::Kind::CreateDataFile:
    case Directory::NamespaceMutation::Kind::CreateDirectory:
    case Directory::NamespaceMutation::Kind::CreateSymlink: {
      if (!destinationID || !profileLogV4FilesystemValidName(
                              mutation.destinationName) ||
          !mutation.subject || subjectID || !validPostImage(
            mutation.destinationParentPostImage,
            ProfileLogV4FilesystemInodeKind::Directory) ||
          !mutation.subjectPostImage ||
          state.inodes.find(destinationID) == state.inodes.end() ||
          state.inodes.at(destinationID).kind !=
            ProfileLogV4FilesystemInodeKind::Directory ||
          state.inodes.at(destinationID).entries.count(mutation.destinationName) ||
          state.nextInode == std::numeric_limits<uint64_t>::max()) {
        return -EIO;
      }
      ProfileLogV4FilesystemInodeKind kind;
      std::string target;
      if (mutation.kind == Directory::NamespaceMutation::Kind::CreateDataFile &&
          mutation.subject->is<ProfileLogV4FilesystemDataFile>()) {
        kind = ProfileLogV4FilesystemInodeKind::Regular;
      } else if (mutation.kind ==
                   Directory::NamespaceMutation::Kind::CreateDirectory &&
                 mutation.subject->is<ProfileLogV4FilesystemDirectory>()) {
        kind = ProfileLogV4FilesystemInodeKind::Directory;
      } else if (mutation.kind ==
                   Directory::NamespaceMutation::Kind::CreateSymlink &&
                 mutation.subject->is<ProfileLogV4FilesystemSymlink>()) {
        kind = ProfileLogV4FilesystemInodeKind::Symlink;
        target = std::static_pointer_cast<ProfileLogV4FilesystemSymlink>(
                   mutation.subject)->getTarget();
        if (target.find('\0') != std::string::npos) {
          return -EINVAL;
        }
      } else {
        return -EIO;
      }
      if (!profileLogV4FilesystemValidMetadata(*mutation.subjectPostImage, kind)) {
        return -EIO;
      }
      const uint64_t newID = state.nextInode++;
      ProfileLogV4FilesystemInode inode;
      inode.id = newID;
      inode.kind = kind;
      inode.metadata = *mutation.subjectPostImage;
      inode.target = std::move(target);
      state.inodes.emplace(newID, std::move(inode));
      state.inodes.at(destinationID).entries.emplace(mutation.destinationName,
                                                      newID);
      state.inodes.at(destinationID).metadata =
        *mutation.destinationParentPostImage;
      if (int error = commitLogicalStateLocked(
            {destinationID, newID}, {}, {})) {
        return error;
      }
      if (kind == ProfileLogV4FilesystemInodeKind::Regular) {
        std::static_pointer_cast<ProfileLogV4FilesystemDataFile>(
          mutation.subject)->adopt(newID);
      } else if (kind == ProfileLogV4FilesystemInodeKind::Directory) {
        std::static_pointer_cast<ProfileLogV4FilesystemDirectory>(
          mutation.subject)->adopt(newID);
      } else {
        std::static_pointer_cast<ProfileLogV4FilesystemSymlink>(
          mutation.subject)->adopt(newID);
      }
      return 0;
    }

    case Directory::NamespaceMutation::Kind::Unlink:
    case Directory::NamespaceMutation::Kind::RemoveDirectory: {
      if (!sourceID || !subjectID || !profileLogV4FilesystemValidName(
                                       mutation.sourceName) ||
          !validPostImage(mutation.sourceParentPostImage,
                          ProfileLogV4FilesystemInodeKind::Directory) ||
          !mutation.subjectPostImage || state.inodes.find(sourceID) ==
            state.inodes.end() || state.inodes.find(subjectID) ==
            state.inodes.end()) {
        return -EIO;
      }
      const auto& source = state.inodes.at(sourceID);
      const auto& subject = state.inodes.at(subjectID);
      if (source.kind != ProfileLogV4FilesystemInodeKind::Directory ||
          source.entries.find(mutation.sourceName) == source.entries.end() ||
          source.entries.at(mutation.sourceName) != subjectID ||
          subjectID == state.root ||
          !profileLogV4FilesystemValidMetadata(
            *mutation.subjectPostImage, subject.kind) ||
          (mutation.kind == Directory::NamespaceMutation::Kind::Unlink &&
           subject.kind == ProfileLogV4FilesystemInodeKind::Directory) ||
          (mutation.kind == Directory::NamespaceMutation::Kind::RemoveDirectory &&
           (subject.kind != ProfileLogV4FilesystemInodeKind::Directory ||
            !subject.entries.empty()))) {
        return -EIO;
      }
      auto removed = subject;
      removed.metadata = *mutation.subjectPostImage;
      state.inodes.at(sourceID).entries.erase(mutation.sourceName);
      state.inodes.at(sourceID).metadata = *mutation.sourceParentPostImage;
      state.inodes.erase(subjectID);
      if (int error = commitLogicalStateLocked(
            {sourceID}, {subjectID}, {}, false)) {
        return error;
      }
      orphans.emplace(subjectID, std::move(removed));
      orphanFiles[subjectID] = mutation.subject;
      return 0;
    }

    case Directory::NamespaceMutation::Kind::Rename: {
      if (!sourceID || !destinationID || !subjectID ||
          !profileLogV4FilesystemValidName(mutation.sourceName) ||
          !profileLogV4FilesystemValidName(mutation.destinationName) ||
          !validPostImage(mutation.sourceParentPostImage,
                          ProfileLogV4FilesystemInodeKind::Directory) ||
          !mutation.subjectPostImage || state.inodes.find(sourceID) ==
            state.inodes.end() || state.inodes.find(destinationID) ==
            state.inodes.end() || state.inodes.find(subjectID) ==
            state.inodes.end()) {
        return -EIO;
      }
      const auto& source = state.inodes.at(sourceID);
      const auto& destination = state.inodes.at(destinationID);
      const auto& subject = state.inodes.at(subjectID);
      if (source.kind != ProfileLogV4FilesystemInodeKind::Directory ||
          destination.kind != ProfileLogV4FilesystemInodeKind::Directory ||
          source.entries.find(mutation.sourceName) == source.entries.end() ||
          source.entries.at(mutation.sourceName) != subjectID ||
          subjectID == state.root ||
          !profileLogV4FilesystemValidMetadata(
            *mutation.subjectPostImage, subject.kind) ||
          (sourceID != destinationID &&
           !validPostImage(mutation.destinationParentPostImage,
                           ProfileLogV4FilesystemInodeKind::Directory))) {
        return -EIO;
      }
      // A full Schema-1 serializer used to catch this through a global graph
      // walk. Normal Schema-2 commits intentionally avoid that O(namespace)
      // pass, so preserve the one affected invariant locally before moving a
      // directory beneath one of its own descendants.
      if (subject.kind == ProfileLogV4FilesystemInodeKind::Directory) {
        std::vector<uint64_t> pendingDirectories = {subjectID};
        std::set<uint64_t> visitedDirectories;
        while (!pendingDirectories.empty()) {
          const uint64_t current = pendingDirectories.back();
          pendingDirectories.pop_back();
          if (!visitedDirectories.emplace(current).second) {
            return -EIO;
          }
          if (current == destinationID) {
            return -EIO;
          }
          const auto currentInode = state.inodes.find(current);
          if (currentInode == state.inodes.end() ||
              currentInode->second.kind !=
                ProfileLogV4FilesystemInodeKind::Directory) {
            return -EIO;
          }
          for (const auto& [_, child] : currentInode->second.entries) {
            const auto childInode = state.inodes.find(child);
            if (childInode == state.inodes.end()) {
              return -EIO;
            }
            if (childInode->second.kind ==
                ProfileLogV4FilesystemInodeKind::Directory) {
              pendingDirectories.push_back(child);
            }
          }
        }
      }
      const auto destinationEntry = destination.entries.find(mutation.destinationName);
      uint64_t removedID = 0;
      std::optional<ProfileLogV4FilesystemInode> removed;
      if (destinationEntry != destination.entries.end() &&
          destinationEntry->second != subjectID) {
        removedID = destinationEntry->second;
        const auto replacement = state.inodes.find(removedID);
        if (replacement == state.inodes.end() || replacementID != removedID ||
            !mutation.replacementPostImage ||
            !profileLogV4FilesystemValidMetadata(
              *mutation.replacementPostImage, replacement->second.kind) ||
            (replacement->second.kind ==
               ProfileLogV4FilesystemInodeKind::Directory &&
             !replacement->second.entries.empty())) {
          return -EIO;
        }
        removed = replacement->second;
        removed->metadata = *mutation.replacementPostImage;
      } else if (mutation.replacement || replacementID) {
        return -EIO;
      }
      state.inodes.at(sourceID).entries.erase(mutation.sourceName);
      state.inodes.at(destinationID).entries.erase(mutation.destinationName);
      state.inodes.at(destinationID).entries.emplace(mutation.destinationName,
                                                      subjectID);
      state.inodes.at(sourceID).metadata = *mutation.sourceParentPostImage;
      if (sourceID != destinationID) {
        state.inodes.at(destinationID).metadata =
          *mutation.destinationParentPostImage;
      }
      state.inodes.at(subjectID).metadata = *mutation.subjectPostImage;
      if (removedID) {
        state.inodes.erase(removedID);
      }
      std::vector<uint64_t> changed = {sourceID, destinationID, subjectID};
      std::sort(changed.begin(), changed.end());
      changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
      if (int error = commitLogicalStateLocked(
            changed, removedID ? std::vector<uint64_t>{removedID}
                               : std::vector<uint64_t>{}, {}, !removedID)) {
        return error;
      }
      if (removedID) {
        orphans.emplace(removedID, std::move(*removed));
        orphanFiles[removedID] = mutation.replacement;
      }
      return 0;
    }
  }
  return -EIO;
}

int ProfileLogV4FilesystemBackend::prepareOPFSProfileRetirement(
  bool checkResources) {
  int firstError = 0;
  {
    ProfileLeaseState::InternalOperation operation(*profileLeaseState);
    if (!operation) {
      firstError = operation.getError();
    } else {
      std::lock_guard<std::recursive_mutex> lock(filesystemMutex);
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE == 1
      if (!checkResources &&
          hasControlledProxyCompletionFailureForTesting()) {
        // The source-selected post-flush publication fault poisons the
        // logical mutation that was in progress. Its V4 envelope has already
        // closed every wrapper/destructor proxy path, however, so the
        // explicit retained-lease disposition must not try a logical flush or
        // turn that known test fault into a second cleanup failure.
      } else
#endif
      if (filesystemFatal) {
        // An inner logical manifest or chunk validation error is not recorded
        // by the outer V4 store, but it is still a profile-integrity failure.
        // Never turn it into a clean lease handoff merely because the envelope
        // itself remains readable.
        firstError = filesystemFatal;
      } else if (logicalStateLoaded) {
        firstError = flushLocked();
      }
    }
  }
  const int inherited = ProfileLogV4Store::prepareOPFSProfileRetirement(
    checkResources && firstError == 0);
  return firstError ? firstError : inherited;
}

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

#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE
int wasmfs_opfs_profile_log_v4_test_proxy_completion_arm(void) {
  return armProfileLogV4ProxyCompletionForTesting();
}

int wasmfs_opfs_profile_log_v4_test_proxy_completion_latch_count(void) {
  return profileLogV4ProxyCompletionLatchCountForTesting.load();
}

int wasmfs_opfs_profile_log_v4_test_proxies_after_latch(void) {
  return profileLogV4ProxiesAfterLatchForTesting.load();
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

backend_t wasmfs_create_opfs_profile_log_v3_data_backend(
  const char* profile_name,
  mode_t payload_mode) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    errno = operation.getError();
    return NullBackend;
  }
  if (!IsValidProfileLeaseName(profile_name) ||
      (payload_mode & ~S_IALLUGO) != 0) {
    errno = EINVAL;
    return NullBackend;
  }

#ifndef __EMSCRIPTEN_PTHREADS__
  // V3's fixed physical files and cooperative profile lease must remain in
  // the dedicated OPFS worker for their whole lifetime.
  errno = ENOTSUP;
  return NullBackend;
#else
  if (!wasmFS.reserveTerminalLeaseOwner()) {
    errno = EBUSY;
    return NullBackend;
  }
  assert(!emscripten_is_main_browser_thread() &&
         "Cannot safely create leased OPFS backend on main browser thread");

  auto backend = std::make_unique<ProfileLogV3DataBackend>();
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
  error = backend->initialise(profile_name, payload_mode);
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

backend_t wasmfs_create_opfs_profile_log_v4_manifest_backend(
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
  // V4's fixed physical files and profile lease must stay in one dedicated
  // worker for their full lifetime. Main-thread Asyncify/JSPI does not provide
  // that ownership boundary.
  errno = ENOTSUP;
  return NullBackend;
#else
  if (!wasmFS.reserveTerminalLeaseOwner()) {
    errno = EBUSY;
    return NullBackend;
  }
  assert(!emscripten_is_main_browser_thread() &&
         "Cannot safely create leased OPFS backend on main browser thread");

  auto backend = std::make_unique<ProfileLogV4ManifestBackend>();
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

backend_t wasmfs_create_opfs_profile_log_v4_filesystem_backend(
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
  // The V4 filesystem needs the same dedicated-worker ownership boundary as
  // the V4 envelope. Main-thread Asyncify/JSPI cannot provide its leased OPFS
  // lifetime or the synchronous profile access-handle semantics.
  errno = ENOTSUP;
  return NullBackend;
#else
  if (!wasmFS.reserveTerminalLeaseOwner()) {
    errno = EBUSY;
    return NullBackend;
  }
  assert(!emscripten_is_main_browser_thread() &&
         "Cannot safely create leased OPFS backend on main browser thread");

  auto backend = std::make_unique<ProfileLogV4FilesystemBackend>();
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
  error = backend->initialiseFilesystem(profile_name);
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

int wasmfs_opfs_profile_log_v4_read_manifest(backend_t backend,
                                             uint8_t* buffer,
                                             size_t capacity,
                                             size_t* size) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    return -operation.getError();
  }
  auto internal = reinterpret_cast<wasmfs::backend_t>(backend);
  if (!size || !wasmFS.ownsBackend(internal)) {
    return -EINVAL;
  }
  if (int error = operation.admitBackend(internal)) {
    return error;
  }
  return internal->readOPFSProfileLogV4Manifest(buffer, capacity, size);
}

int wasmfs_opfs_profile_log_v4_commit_manifest(backend_t backend,
                                               const uint8_t* data,
                                               size_t size) {
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
  return internal->commitOPFSProfileLogV4Manifest(data, size);
}

void EMSCRIPTEN_KEEPALIVE _wasmfs_opfs_record_entry(
  std::vector<Directory::Entry>* entries, const char* name, int type) {
  entries->push_back({name, File::FileKind(type), 0});
}

} // extern "C"
