// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the global state.

#include <emscripten/threading.h>
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

thread_local WasmFS* WasmFS::activeOperationWasmFS = nullptr;
thread_local size_t WasmFS::activeOperationDepth = 0;
thread_local WasmFS* WasmFS::terminalStdioFlushWasmFS = nullptr;

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
  // must be able to finish buffered FILE output after the terminal fence is
  // closed, but that exception must not become a general same-thread bypass:
  // in particular, a fopencookie callback cannot create a backend or open
  // another file while its stream is being flushed. Only the fd-write
  // entrypoint opts into this uncounted, drain-thread-local admission. A
  // direct write from a custom callback is not distinguishable at this ABI, so
  // the public contract requires custom callbacks to make no WasmFS calls.
  if (kind == Kind::StdioFlushWrite &&
      terminalStdioFlushWasmFS == &wasmfs) {
    admitted = true;
    return;
  }

  // Reentering through another public WasmFS entrypoint is part of the same
  // admitted operation. In particular, several POSIX wrappers delegate to
  // other syscall implementations.
  if (activeOperationWasmFS == &wasmfs) {
    ++activeOperationDepth;
    admitted = true;
    tracksDepth = true;
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
  admitted = true;
  tracksDepth = true;
  ownsActiveOperation = true;
}

WasmFS::Operation::~Operation() {
  if (!admitted || !tracksDepth) {
    return;
  }
  assert(activeOperationWasmFS == wasmfs);
  assert(activeOperationDepth > 0);
  if (--activeOperationDepth != 0) {
    return;
  }

  assert(ownsActiveOperation);
  activeOperationWasmFS = nullptr;
  std::unique_lock<std::mutex> lock(wasmfs->operationMutex);
  assert(wasmfs->activeOperations > 0);
  --wasmfs->activeOperations;
  if (wasmfs->activeOperations == 0) {
    wasmfs->operationCV.notify_all();
  }
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
  return true;
}

void WasmFS::cancelTerminalLeaseOwnerReservation() {
  const std::lock_guard<std::mutex> lock(mutex);
  assert(terminalLeaseOwnerReserved);
  terminalLeaseOwnerReserved = false;
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
  if (emscripten_is_main_runtime_thread()) {
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
  assert(!terminalStdioFlushWasmFS);
  terminalStdioFlushWasmFS = this;
  errno = 0;
  if (fflush(nullptr) == EOF) {
    recordDrainError(*result,
                     errno ? -errno : -EIO,
                     result->libc_flush_failed);
  }
  assert(terminalStdioFlushWasmFS == this);
  terminalStdioFlushWasmFS = nullptr;

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

extern "C" int wasmfs_terminal_drain(
  wasmfs_terminal_drain_result* result) {
  return wasmFS.terminalDrain(result);
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

  // If the /dev/ directory does not already exist, create it. (It may already
  // exist in NODERAWFS mode, or if those files have been preloaded.)
  auto devDir = lockedRoot.insertDirectory("dev", S_IRUGO | S_IXUGO);
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
  lockedRoot.insertDirectory("tmp", S_IRWXUGO);

  return rootDirectory;
}

// Initialize files specified by the --preload-file option.
// Set up directories and files in wasmFS$preloadedDirs and
// wasmFS$preloadedFiles from JS. This function will be called before any file
// operation to ensure any preloaded files are eagerly available for use.
void WasmFS::preloadFiles() {
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
    if (lockedParentDir.getChild(childName)) {
      // The child already exists, so we don't need to do anything here.
      continue;
    }

    auto inserted =
      lockedParentDir.insertDirectory(childName, S_IRUGO | S_IXUGO);
    assert(inserted && "TODO: handle preload insertion errors");
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
    auto created =
      parent->locked().insertDataFile(std::string(childName), (mode_t)mode);
    assert(created && "TODO: handle preload insertion errors");
    created->locked().preloadFromJS(i);
  }
}

} // namespace wasmfs
