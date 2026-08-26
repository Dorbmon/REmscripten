// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the global state.

#include <emscripten/threading.h>
#include <emscripten/wasmfs_opfs_profile_drain.h>
#include <algorithm>
#include <errno.h>
#include <new>

#include "memory_backend.h"
#include "paths.h"
#include "special_files.h"
#include "wasmfs.h"
#include "wasmfs_internal.h"

// Keep the C system-library bridge opaque. emscripten_mmap.c uses its weak
// declarations only when full WasmFS has been linked, so a no-FS program does
// not gain a dependency on this object.
struct wasmfs_operation_handle {
  wasmfs::WasmFS::Operation operation;

  wasmfs_operation_handle() : operation(wasmfs::wasmFS) {}
};

extern "C" wasmfs_operation_handle* __wasmfs_acquire_operation(void) {
  auto* handle = new (std::nothrow) wasmfs_operation_handle();
  if (!handle) {
    errno = ENOMEM;
    return nullptr;
  }
  if (!handle->operation) {
    errno = handle->operation.getError();
    delete handle;
    return nullptr;
  }
  return handle;
}

extern "C" void
__wasmfs_release_operation(wasmfs_operation_handle* handle) {
  delete handle;
}

namespace wasmfs {

#ifdef WASMFS_OPFS_PROFILE_DRAIN_TEST
namespace {

// These controls are intentionally internal to the test-only libwasmfs
// variation. They stop a scoped drain immediately after sealing so the test
// can distinguish ESHUTDOWN before filtered detach from EBADF afterwards.
std::atomic<int> profileDrainAfterSealTestState = 0;
// This one stops the next OPFS file write after its outer syscall has acquired
// the profile backend token but before the OPFS virtual method admits itself.
// It exercises the pre-seal continuity edge: beginDrain must wait for that
// token, and the resumed virtual call must not be rejected merely because the
// state changed to Sealing meanwhile.
std::atomic<int> profileDrainFileOperationTestState = 0;
std::atomic<int> profileDrainSealingTestState = 0;

void pauseProfileDrainAfterSealForTesting() {
  int expected = 1;
  if (!profileDrainAfterSealTestState.compare_exchange_strong(expected, 2)) {
    return;
  }
  while (profileDrainAfterSealTestState.load() == 2) {
    emscripten_thread_sleep(1);
  }
  assert(profileDrainAfterSealTestState.load() == 3);
  profileDrainAfterSealTestState.store(0);
}

void pauseProfileDrainFileOperationForTesting() {
  int expected = 1;
  if (!profileDrainFileOperationTestState.compare_exchange_strong(expected,
                                                                    2)) {
    return;
  }
  while (profileDrainFileOperationTestState.load() == 2) {
    emscripten_thread_sleep(1);
  }
  assert(profileDrainFileOperationTestState.load() == 3);
  profileDrainFileOperationTestState.store(0);
}

} // anonymous namespace

extern "C" void wasmfs_opfs_profile_drain_test_arm_after_seal(void) {
  int expected = 0;
  assert(profileDrainAfterSealTestState.compare_exchange_strong(expected, 1));
}

extern "C" int wasmfs_opfs_profile_drain_test_after_seal_state(void) {
  return profileDrainAfterSealTestState.load();
}

extern "C" void wasmfs_opfs_profile_drain_test_continue_after_seal(void) {
  int expected = 2;
  assert(profileDrainAfterSealTestState.compare_exchange_strong(expected, 3));
}

extern "C" void wasmfs_opfs_profile_drain_test_arm_file_operation(void) {
  int expected = 0;
  assert(profileDrainFileOperationTestState.compare_exchange_strong(expected,
                                                                      1));
}

extern "C" int wasmfs_opfs_profile_drain_test_file_operation_state(void) {
  return profileDrainFileOperationTestState.load();
}

extern "C" void wasmfs_opfs_profile_drain_test_continue_file_operation(void) {
  int expected = 2;
  assert(profileDrainFileOperationTestState.compare_exchange_strong(expected,
                                                                      3));
}

extern "C" void wasmfs_opfs_profile_drain_test_maybe_block_file_operation() {
  pauseProfileDrainFileOperationForTesting();
}

extern "C" void wasmfs_opfs_profile_drain_test_reset_sealing(void) {
  profileDrainSealingTestState.store(0);
}

extern "C" void wasmfs_opfs_profile_drain_test_note_sealing(void) {
  profileDrainSealingTestState.store(1);
}

extern "C" int wasmfs_opfs_profile_drain_test_sealing_state(void) {
  return profileDrainSealingTestState.load();
}
#endif

thread_local WasmFS* WasmFS::activeOperationWasmFS = nullptr;
thread_local size_t WasmFS::activeOperationDepth = 0;
thread_local WasmFS::Operation* WasmFS::activeOperationRoot = nullptr;
thread_local WasmFS* WasmFS::stdioFlushWasmFS = nullptr;
thread_local backend_t WasmFS::stdioFlushBackend = NullBackend;

#ifdef WASMFS_CASE_INSENSITIVE
backend_t createIgnoreCaseBackend(std::function<backend_t()> createBackend);
#endif

// The below lines are included to make the compiler believe that the global
// constructor is part of a system header, which is necessary to work around a
// compilation error about using a reserved init priority less than 101. This
// ensures that the global state of the file system is constructed before
// anything else. ATTENTION: No other static global objects should be defined
// besides wasmFS. Due to # define _LIBCPP_INIT_PRIORITY_MAX
// __attribute__((init_priority(101))), we must use init priority 100 (reserved
// system priority) since wasmFS is a system level component.
// TODO: consider instead adding this in libc's startup code.
// WARNING: Maintain # n + 1 "wasmfs.cpp" 3 where n = line number.
# 29 "wasmfs.cpp" 3
__attribute__((init_priority(100))) WasmFS wasmFS;
# 31 "wasmfs.cpp"

WasmFS::Operation::Operation(WasmFS& wasmfs, Kind kind) : wasmfs(&wasmfs) {
  // fflush(NULL) reaches the descriptor table through __wasi_fd_write(). It
  // must be able to finish buffered FILE output after a drain fence closes,
  // but that exception must not become a broad same-thread Operation: a
  // fopencookie callback must not be able to inherit it to open paths or
  // create backends. The special entrypoint later admits only its existing fd
  // backend. A direct custom-callback write remains ABI-indistinguishable from
  // libc's write and is documented as unsupported by the drain contract.
  if (stdioFlushWasmFS == &wasmfs) {
    if (kind != Kind::StdioFlushWrite) {
      error = ESHUTDOWN;
      return;
    }
    admitted = true;
    // A libc callback can itself issue a second direct fd write while the
    // first special write is live. Share its narrowly scoped token rather
    // than replacing the root and accidentally clearing it on the inner
    // return. No General operation may use this route.
    if (activeOperationRoot) {
      rootOperation = activeOperationRoot;
      return;
    }
    stdioFlushBypass = true;
    activeOperationRoot = this;
    return;
  }

  // Reentering through another public WasmFS entrypoint is part of the same
  // admitted operation. In particular, several POSIX wrappers delegate to
  // other syscall implementations.
  if (activeOperationWasmFS == &wasmfs) {
    ++activeOperationDepth;
    admitted = true;
    tracksDepth = true;
    rootOperation = activeOperationRoot;
    return;
  }

  // WasmFS has one process-global instance. Do not let an internal test-only
  // secondary instance accidentally inherit admission to that global instance.
  if (activeOperationWasmFS) {
    error = EDEADLK;
    return;
  }

  std::unique_lock<std::mutex> lock(wasmfs.operationMutex);
  if (wasmfs.terminalState != WasmFS::TerminalState::Running) {
    error = ESHUTDOWN;
    return;
  }
  ++wasmfs.activeOperations;
  activeOperationWasmFS = &wasmfs;
  activeOperationDepth = 1;
  activeOperationRoot = this;
  admitted = true;
  tracksDepth = true;
  ownsActiveOperation = true;
}

WasmFS::Operation::~Operation() {
  if (!admitted) {
    return;
  }

  if (stdioFlushBypass) {
    for (auto backend : admittedBackends) {
      backend->releaseProfileOperation();
    }
    assert(activeOperationRoot == this);
    activeOperationRoot = nullptr;
    return;
  }

  if (!tracksDepth) {
    return;
  }
  assert(activeOperationWasmFS == wasmfs);
  assert(activeOperationDepth > 0);
  if (--activeOperationDepth != 0) {
    return;
  }

  assert(ownsActiveOperation);
  for (auto backend : admittedBackends) {
    backend->releaseProfileOperation();
  }
  assert(activeOperationRoot == this);
  activeOperationRoot = nullptr;
  activeOperationWasmFS = nullptr;
  std::unique_lock<std::mutex> lock(wasmfs->operationMutex);
  assert(wasmfs->activeOperations > 0);
  --wasmfs->activeOperations;
  if (wasmfs->activeOperations == 0) {
    wasmfs->operationCV.notify_all();
  }
}

int WasmFS::Operation::admitBackend(backend_t backend) {
  if (!backend) {
    return 0;
  }
  if (!admitted) {
    return error ? -error : -EDEADLK;
  }
  if (rootOperation) {
    return rootOperation->admitBackend(backend);
  }

  // A terminal drain accepts all direct libc fd writes while its global fence
  // is active. A scoped profile drain accepts that bypass only for its exact
  // sealed backend; unrelated existing descriptors retain their ordinary
  // backend admission without turning this into a reentrant outer operation.
  if (stdioFlushBypass &&
      (stdioFlushBackend == NullBackend || backend == stdioFlushBackend)) {
    return 0;
  }

  if (std::find(admittedBackends.begin(), admittedBackends.end(), backend) !=
      admittedBackends.end()) {
    return 0;
  }
  if (int err = backend->acquireProfileOperation()) {
    return err;
  }
  admittedBackends.push_back(backend);
  return 0;
}

int WasmFS::admitBackend(backend_t backend) {
  if (!backend) {
    return 0;
  }
  if (!activeOperationRoot) {
    return -EDEADLK;
  }
  return activeOperationRoot->admitBackend(backend);
}

bool WasmFS::ownsBackend(backend_t backend) {
  if (!backend) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(mutex);
  return std::any_of(backendTable.begin(), backendTable.end(), [&](auto& item) {
    return item.get() == backend;
  });
}

int WasmFS::beginScopedOPFSProfileDrain() {
  std::unique_lock<std::mutex> lock(operationMutex);
  if (terminalState != TerminalState::Running) {
    return terminalState == TerminalState::Draining ? -EBUSY : -ESHUTDOWN;
  }
  if (scopedProfileDrainInProgress) {
    return -EBUSY;
  }
  scopedProfileDrainInProgress = true;
  return 0;
}

void WasmFS::endScopedOPFSProfileDrain() {
  std::unique_lock<std::mutex> lock(operationMutex);
  assert(scopedProfileDrainInProgress);
  scopedProfileDrainInProgress = false;
  operationCV.notify_all();
}

backend_t WasmFS::addBackend(std::unique_ptr<Backend> backend) {
  Operation operation(*this);
  if (!operation) {
    errno = operation.getError();
    return NullBackend;
  }
  const std::lock_guard<std::mutex> lock(mutex);
  backendTable.push_back(std::move(backend));
  return backendTable.back().get();
}

bool WasmFS::reserveTerminalLeaseOwner() {
  const std::lock_guard<std::mutex> lock(mutex);
  if (terminalLeaseOwnerReserved) {
    return false;
  }
  terminalLeaseOwnerReserved = true;
  terminalLeaseOwnerReservationAmbiguous = false;
  return true;
}

void WasmFS::cancelTerminalLeaseOwnerReservation() {
  const std::lock_guard<std::mutex> lock(mutex);
  assert(terminalLeaseOwnerReserved);
  assert(!terminalLeaseOwnerReservationAmbiguous);
  terminalLeaseOwnerReserved = false;
}

void WasmFS::markTerminalLeaseOwnerReservationAmbiguous() {
  const std::lock_guard<std::mutex> lock(mutex);
  assert(terminalLeaseOwnerReserved);
  terminalLeaseOwnerReservationAmbiguous = true;
}

namespace {

void flushWasmFSStreams() {
  // Flush musl libc streams before the descriptor table is detached. The
  // terminal drain admits this only on its own thread after all other WasmFS
  // operations have quiesced.
  fflush(nullptr);

  // Flush our own streams. TODO: flush all backends.
  (void)SpecialFiles::getStdout()->locked().flush();
  (void)SpecialFiles::getStderr()->locked().flush();
}

int normalizeDrainError(int error) {
  if (error < 0) {
    return error;
  }
  if (error > 0) {
    return -error;
  }
  return -EIO;
}

void recordDrainError(wasmfs_terminal_drain_result& result,
                      int error,
                      uint32_t& counter) {
  ++counter;
  if (result.error == 0) {
    result.error = normalizeDrainError(error);
  }
}

void recordProfileDrainError(wasmfs_opfs_profile_drain_result& result,
                             int error,
                             uint32_t& counter) {
  ++counter;
  if (result.error == 0) {
    result.error = normalizeDrainError(error);
  }
}

void setProfileDrainError(wasmfs_opfs_profile_drain_result& result,
                          int error) {
  if (result.error == 0) {
    result.error = normalizeDrainError(error);
  }
}

} // anonymous namespace

// If the user does not implement this hook, do nothing.
__attribute__((weak)) extern "C" void wasmfs_before_preload(void) {}

// Set up global data structures and preload files.
WasmFS::WasmFS() : rootDirectory(initRootDirectory()), cwd(rootDirectory) {
  wasmfs_before_preload();
  preloadFiles();
}

// Manual integration with LSan. LSan installs itself during startup at the
// first allocation, which happens inside WasmFS code (since the WasmFS global
// object creates some data structures). As a result LSan's atexit() destructor
// will be called last, after WasmFS is cleaned up, since atexit() calls work
// are LIFO (like a stack). But that is a problem, since if WasmFS has shut
// down and deallocated itself then the leak code cannot actually print any of
// its findings, if it has any. To avoid that, define the LSan entry point as a
// weak symbol, and call it; if LSan is not enabled this can be optimized out,
// and if LSan is enabled then we'll check for leaks right at the start of the
// WasmFS destructor, which is the last time during which it is valid to print.
// (Note that this means we can't find leaks inside WasmFS code itself, but that
// seems fundamentally impossible for the above reasons, unless we made LSan log
// its findings in a way that does not depend on normal file I/O.)
__attribute__((weak)) extern "C" void __lsan_do_leak_check(void) {}

extern "C" void wasmfs_flush(void) {
  WasmFS::Operation operation(wasmFS);
  if (!operation) {
    errno = operation.getError();
    return;
  }
  flushWasmFSStreams();
}

int WasmFS::terminalDrain(wasmfs_terminal_drain_result* result) {
  if (!result) {
    return -EINVAL;
  }
  *result = {};

  // A terminal drain can wait for filesystem operations and can synchronously
  // proxy backend closes. The Emscripten runtime's main thread cannot block
  // that way: in browsers it services the JavaScript event loop and, in Node,
  // it is likewise the default runtime thread. Run application teardown on a
  // pthread instead.
  // With PROXY_TO_PTHREAD the runtime main is an application Worker, not the
  // browser's JavaScript main thread. Both are invalid here: this path can
  // synchronously proxy and a leased OPFS finalizer may cancel/join its
  // dedicated Worker.
  if (emscripten_is_main_runtime_thread() ||
      emscripten_is_main_browser_thread()) {
    return -EAGAIN;
  }

  // An admitted operation is waiting for this call to finish, so waiting for
  // all admitted operations here would deadlock. A second WasmFS instance is
  // internal/test-only and must not borrow this global instance's admission.
  if (activeOperationWasmFS) {
    return -EDEADLK;
  }

  {
    std::unique_lock<std::mutex> lock(operationMutex);
    if (scopedProfileDrainInProgress) {
      return -EBUSY;
    }
    if (terminalState == TerminalState::Draining) {
      return -EBUSY;
    }
    if (terminalState != TerminalState::Running) {
      return -ESHUTDOWN;
    }
    terminalState = TerminalState::Draining;
    operationCV.wait(lock, [&] { return activeOperations == 0; });
  }

  // The only temporarily admitted operation after the state transition is
  // direct fd output from libc's flush sequence on this thread. No other
  // public WasmFS operation can enter now, including a backend factory called
  // by a custom fopencookie callback.
  assert(!stdioFlushWasmFS);
  assert(stdioFlushBackend == NullBackend);
  stdioFlushWasmFS = this;
  errno = 0;
  if (fflush(nullptr) == EOF) {
    recordDrainError(*result,
                     errno ? -errno : -EIO,
                     result->libc_flush_failed);
  }
  assert(stdioFlushWasmFS == this);
  stdioFlushWasmFS = nullptr;

  std::vector<std::shared_ptr<DataFile>> closees;
  {
    // Detach first and close later: a close can proxy to the browser and must
    // never run while the descriptor-table mutex is held.
    auto fileTable = getFileTable().locked();
    closees = fileTable.detachAll();
  }

  result->data_file_states = static_cast<uint32_t>(closees.size());
  for (auto& closee : closees) {
    auto file = closee->locked();
    if (int error = file.flush()) {
      recordDrainError(*result, error, result->data_flush_failures);
    }
    // Always attempt close, including after a flush failure. In particular,
    // OPFS close failure is itself significant and must not be hidden by an
    // earlier flush error.
    if (int error = file.close()) {
      recordDrainError(*result, error, result->data_close_failures);
    }
  }

  // A failed leased-OPFS factory can retain a dedicated worker before it has
  // an object in backendTable. Its reservation is deliberately fail-closed;
  // make that hidden owner result-bearing as well, so terminal drain cannot
  // report success while a browser Web Lock or access handle may remain live.
  bool unrepresentedLeaseOwner = false;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    unrepresentedLeaseOwner = terminalLeaseOwnerReservationAmbiguous;
  }
  if (unrepresentedLeaseOwner) {
    recordDrainError(*result,
                     -ESHUTDOWN,
                     result->backend_terminal_failures);
  }

  // The table is terminal before notifying backends, and the public-operation
  // state intentionally remains Draining until every result-bearing backend
  // finalizer has completed. A backend hook therefore cannot use its
  // notification as an opportunity to start new filesystem work.
  std::vector<Backend*> backends;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    backends.reserve(backendTable.size());
    for (const auto& backend : backendTable) {
      backends.push_back(backend.get());
    }
  }
  auto finishBackends = [&](bool leaseOwners) {
    for (auto* backend : backends) {
      if (backend->releasesTerminalLease() != leaseOwners) {
        continue;
      }
      // A failed ordinary backend finalizer prevents a later cooperative
      // lease owner from releasing its lease. Keep going so all terminal
      // cleanup receives a definitive false result and every observable
      // backend failure is counted.
      if (int error = backend->terminalDrainFinished(result->error == 0)) {
        recordDrainError(*result, error, result->backend_terminal_failures);
      }
    }
  };
  finishBackends(false);
  // Run lease owners last: a successful OPFS lease release is meaningful only
  // after all non-lease terminal cleanup has succeeded. There is intentionally
  // no multi-lease transaction. WasmFS enforces one cooperative profile lease
  // owner per instance before its backend is constructed.
  finishBackends(true);

  {
    std::unique_lock<std::mutex> lock(operationMutex);
    terminalState = result->error == 0 ? TerminalState::Drained
                                       : TerminalState::Failed;
  }

  return result->error;
}

int WasmFS::drainOPFSProfileBackend(
  backend_t backend, wasmfs_opfs_profile_drain_result* result) {
  if (!result) {
    return -EINVAL;
  }
  *result = {};

  auto returnError = [&](int error) {
    setProfileDrainError(*result, error);
    return result->error;
  };

  if (!backend) {
    return returnError(-EINVAL);
  }
  // Do not dereference an arbitrary opaque pointer. A caller may retain only
  // a backend returned by this WasmFS instance, and an unowned pointer is not
  // a candidate for the leased-OPFS protocol.
  if (!ownsBackend(backend) || !backend->isLeasedOPFSProfileBackend()) {
    return returnError(-ENOTSUP);
  }
  // `PROXY_TO_PTHREAD` makes runtime-main and browser-main distinct. Scoped
  // retirement synchronously cancels/joins the dedicated OPFS Worker, so the
  // public ABI must reject either main context before it seals a backend.
  if (emscripten_is_main_runtime_thread() ||
      emscripten_is_main_browser_thread()) {
    return returnError(-EAGAIN);
  }
  // Waiting for an outer operation that is itself waiting for this call would
  // deadlock. The special fflush write operation is only ever nested inside a
  // drain that has already passed this check.
  if (activeOperationWasmFS || activeOperationRoot) {
    return returnError(-EDEADLK);
  }

  if (int error = beginScopedOPFSProfileDrain()) {
    return returnError(error);
  }

  int beginError = backend->beginOPFSProfileDrain();
  if (beginError) {
    endScopedOPFSProfileDrain();
    return returnError(beginError);
  }
  result->backend_sealed = 1;

#ifdef WASMFS_OPFS_PROFILE_DRAIN_TEST
  pauseProfileDrainAfterSealForTesting();
#endif

  // A close that failed before this drain has already removed its descriptor,
  // so snapshot it before this drain's own close attempts. It blocks release
  // but is not double-counted if a later close in this drain also fails.
  if (int error = backend->getOPFSProfilePriorCloseError()) {
    recordProfileDrainError(*result, error, result->prior_close_failures);
  }

  // Do not create a normal outer Operation around fflush: a custom stream
  // callback would inherit it and gain broad reentrant path/factory access.
  // Only __wasi_fd_write receives the narrow special admission below, and it
  // may bypass the sealed backend only for an already-open target descriptor.
  assert(!stdioFlushWasmFS);
  assert(stdioFlushBackend == NullBackend);
  stdioFlushWasmFS = this;
  stdioFlushBackend = backend;
  errno = 0;
  if (fflush(nullptr) == EOF) {
    recordProfileDrainError(*result,
                            errno ? -errno : -EIO,
                            result->libc_flush_failed);
  }
  assert(stdioFlushWasmFS == this);
  assert(stdioFlushBackend == backend);
  stdioFlushBackend = NullBackend;
  stdioFlushWasmFS = nullptr;

  std::vector<std::shared_ptr<DataFile>> closees;
  {
    // Detach aliases while the table is locked, then flush/close after
    // releasing it because an OPFS close can synchronously proxy to JS.
    auto fileTable = getFileTable().locked();
    closees = fileTable.detachBackend(backend, result->detached_descriptors);
  }

  result->data_file_states = static_cast<uint32_t>(closees.size());
  for (auto& closee : closees) {
    auto file = closee->locked();
    if (int error = file.flush()) {
      recordProfileDrainError(*result, error, result->data_flush_failures);
    }
    if (int error = file.close()) {
      recordProfileDrainError(*result, error, result->data_close_failures);
    }
  }

  // Fence/preflight browser-owned OPFS state before releasing Web Locks. A
  // preflight failure is an ordinary failed drain: the backend retains its
  // lease rather than clearing handles or reporting a handoff that did not
  // happen.
  bool cleanupSucceeded = result->error == 0;
  {
    if (int error = backend->prepareOPFSProfileRetirement(cleanupSucceeded)) {
      recordProfileDrainError(*result, error, result->backend_retire_failures);
    }
  }

  // The backend executes release, OPFS-context reset, and heartbeat stop in
  // one dedicated-worker transaction. `leaseReleased` is separate because a
  // later cleanup error can occur only after Web Locks has already
  // acknowledged release. Native worker retirement still runs in that case
  // so a non-success result never defers a join to browser-main destruction.
  bool leaseReleased = false;
  int finishError = backend->finishOPFSProfileDrain(
    result->error == 0, &leaseReleased);
  if (!leaseReleased) {
    // A leased backend must never turn an absent acknowledgement into a
    // zero-result handoff merely because an implementation forgot to surface
    // its own error. Keep this defense at the generic ABI boundary rather
    // than relying on one JavaScript callback's current error mapping.
    if (finishError == 0 && result->error == 0) {
      finishError = -EIO;
    }
    if (finishError) {
      recordProfileDrainError(
        *result, finishError, result->lease_release_failures);
    }
  } else {
    result->lease_released = 1;
    if (finishError) {
      recordProfileDrainError(
        *result, finishError, result->backend_retire_failures);
    }
    if (int error = backend->retireOPFSProfileBackend(finishError == 0)) {
      recordProfileDrainError(*result, error, result->backend_retire_failures);
    } else if (finishError == 0) {
      result->backend_retired = 1;
    }
  }

  endScopedOPFSProfileDrain();
  return result->error;
}

extern "C" int wasmfs_terminal_drain(
  wasmfs_terminal_drain_result* result) {
  return wasmFS.terminalDrain(result);
}

extern "C" int wasmfs_drain_opfs_profile_backend(
  ::backend_t backend, wasmfs_opfs_profile_drain_result* result) {
  // The public C opaque pointer and WasmFS's C++ backend_t have intentionally
  // distinct declarations. They are ABI-identical opaque pointers, but keep
  // the conversion at this C bridge rather than exposing C types internally.
  return wasmFS.drainOPFSProfileBackend(
    reinterpret_cast<wasmfs::backend_t>(backend), result);
}

WasmFS::~WasmFS() {
  // See comment above on this function.
  //
  // Note that it is ok for us to call it here, as LSan internally makes all
  // calls after the first into no-ops. That is, calling it here makes the one
  // time that leak checks are run be done here, or potentially earlier, but not
  // later; and as mentioned in the comment above, this is the latest possible
  // time for the checks to run (since right after this nothing can be printed).
  __lsan_do_leak_check();

  // TODO: Integrate musl exit() which would flush the libc part for us. That
  //       might also help with destructor priority - we need to happen last.
  //       (But we would still need to flush the internal WasmFS buffers, see
  //       wasmfs_flush() and the comment on it in the header.)
  flushWasmFSStreams();

  // Break the reference cycle caused by the root directory being its own
  // parent.
  rootDirectory->locked().setParent(nullptr);
}

// Special backends that want to install themselves as the root use this hook.
// Otherwise, we use the default backends.
__attribute__((weak)) extern "C" backend_t wasmfs_create_root_dir(void) {
#ifdef WASMFS_CASE_INSENSITIVE
  return createIgnoreCaseBackend([]() { return createMemoryBackend(); });
#else
  return createMemoryBackend();
#endif
}

std::shared_ptr<Directory> WasmFS::initRootDirectory() {
  auto rootBackend = wasmfs_create_root_dir();
  auto rootDirectory =
    rootBackend->createDirectory(S_IRUGO | S_IXUGO | S_IWUGO);
  auto lockedRoot = rootDirectory->locked();

  // The root directory is its own parent.
  lockedRoot.setParent(rootDirectory);

  const bool atomicNamespace =
    rootBackend->requiresAtomicNamespaceMutations();
  auto createRootChildDirectory = [&](const char* name, mode_t mode) {
    // The legacy insertion hooks historically tolerate existing root children
    // supplied by NODERAWFS or preload setup. An atomic persistent root must
    // discover and reuse such a directory before attempting a transaction,
    // rather than treating its durable EEXIST as a startup failure.
    if (atomicNamespace) {
      auto lookup = lockedRoot.getChildWithError(name);
      if (lookup.getError()) {
        emscripten_err("Fatal error during root directory lookup in WasmFS.");
        abort();
      }
      if (auto existing = lookup.getFile()) {
        auto directory = existing->dynCast<Directory>();
        if (!directory) {
          emscripten_err("Fatal non-directory root child in WasmFS.");
          abort();
        }
        return directory;
      }
    }

    std::shared_ptr<Directory> result;
    int error = lockedRoot.insertDirectoryWithNamespaceTransaction(
      name, mode, result);
    if (error && atomicNamespace) {
      emscripten_err("Fatal error during root directory initialization in WasmFS.");
      abort();
    }
    return result;
  };

  // If the /dev/ directory does not already exist, create it. (It may already
  // exist in NODERAWFS mode, or if those files have been preloaded.)
  auto devDir = createRootChildDirectory("dev", S_IRUGO | S_IXUGO);
  if (devDir) {
    auto lockedDev = devDir->locked();
    lockedDev.mountChild("null", SpecialFiles::getNull());
    lockedDev.mountChild("stdin", SpecialFiles::getStdin());
    lockedDev.mountChild("stdout", SpecialFiles::getStdout());
    lockedDev.mountChild("stderr", SpecialFiles::getStderr());
    lockedDev.mountChild("random", SpecialFiles::getRandom());
    lockedDev.mountChild("urandom", SpecialFiles::getURandom());
  }

  // As with the /dev/ directory, it is not an error for /tmp/ to already
  // exist.
  [[maybe_unused]] auto tmpDir =
    createRootChildDirectory("tmp", S_IRWXUGO);

  return rootDirectory;
}

// Initialize files specified by the --preload-file option.
// Set up directories and files in wasmFS$preloadedDirs and
// wasmFS$preloadedFiles from JS. This function will be called before any file
// operation to ensure any preloaded files are eagerly available for use.
void WasmFS::preloadFiles() {
  // Preload hooks use the same path helpers as public entrypoints, including
  // their backend admission checks. Keep the initialization work explicitly
  // inside one internal operation so it cannot be mistaken for an unadmitted
  // direct backend access.
  Operation operation(*this);
  assert(operation);

  // Debug builds only: add check to ensure preloadFiles() is called once.
#ifndef NDEBUG
  static std::atomic<int> timesCalled;
  timesCalled++;
  assert(timesCalled == 1);
#endif

  // Ensure that files are preloaded from the main thread.
  assert(emscripten_is_main_runtime_thread());

  auto numFiles = _wasmfs_get_num_preloaded_files();
  auto numDirs = _wasmfs_get_num_preloaded_dirs();

  // If there are no preloaded files, exit early.
  if (numDirs == 0 && numFiles == 0) {
    return;
  }

  // Iterate through wasmFS$preloadedDirs to obtain a parent and child pair.
  // Ex. Module['FS_createPath']("/foo/parent", "child", true, true);
  for (int i = 0; i < numDirs; i++) {
    char parentPath[PATH_MAX] = {};
    _wasmfs_get_preloaded_parent_path(i, parentPath);

    auto parsed = path::parseFile(parentPath);
    std::shared_ptr<Directory> parentDir;
    if (parsed.getError() ||
        !(parentDir = parsed.getFile()->dynCast<Directory>())) {
      emscripten_err(
        "Fatal error during directory creation in file preloading.");
      abort();
    }

    char childName[PATH_MAX] = {};
    _wasmfs_get_preloaded_child_path(i, childName);

    auto lockedParentDir = parentDir->locked();
    auto child = lockedParentDir.getChildWithError(childName);
    if (child.getError()) {
      emscripten_err(
        "Fatal error during directory lookup in file preloading.");
      abort();
    }
    if (child.getFile()) {
      // The child already exists, so we don't need to do anything here.
      continue;
    }

    std::shared_ptr<Directory> inserted;
    int error = lockedParentDir.insertDirectoryWithNamespaceTransaction(
      childName, S_IRUGO | S_IXUGO, inserted);
    if (error && parentDir->getBackend() &&
        parentDir->getBackend()->requiresAtomicNamespaceMutations()) {
      emscripten_err(
        "Fatal error during directory creation in file preloading.");
      abort();
    }
    // Keep the historical legacy-backend assertion behavior. Atomic namespace
    // backends above abort rather than silently proceeding after a failed
    // transaction, including the default ENOTSUP hook.
    assert(!error && inserted && "TODO: handle preload insertion errors");
  }

  for (int i = 0; i < numFiles; i++) {
    char fileName[PATH_MAX] = {};
    _wasmfs_get_preloaded_path_name(i, fileName);

    auto mode = _wasmfs_get_preloaded_file_mode(i);

    auto parsed = path::parseParent(fileName);
    if (parsed.getError()) {
      emscripten_err("Fatal error during file preloading");
      abort();
    }
    auto& [parent, childName] = parsed.getParentChild();
    std::shared_ptr<DataFile> created;
    int error;
    {
      auto lockedParent = parent->locked();
      error = lockedParent.insertDataFileWithNamespaceTransaction(
        std::string(childName), (mode_t)mode, created);
    }
    if (error && parent->getBackend() &&
        parent->getBackend()->requiresAtomicNamespaceMutations()) {
      emscripten_err("Fatal error during file creation in file preloading.");
      abort();
    }
    // See the directory case above: preserve the legacy assertion while
    // making an opted-in backend's failed transaction terminal.
    assert(!error && created && "TODO: handle preload insertion errors");
    if (created->locked().preloadFromJS(i)) {
      emscripten_err("Fatal error during file data preloading.");
      abort();
    }
  }
}

} // namespace wasmfs
