/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

enum TestRole {
  kHolder,
  kContender,
};

enum TestResult {
  kReady,
  kBusy,
  kOtherFailure,
};

static const char kProfileName[] = "wasmfs-terminal-drain-lease";
#ifdef WASMFS_OPFS_TERMINAL_DRAIN_HOLDER
static const char kSecondProfileName[] = "wasmfs-terminal-drain-second";
#if defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN) || \
  defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
static const char kFilePath[] = "/opfs/wasmfs-terminal-drain-close";
#endif
static _Atomic int shutdown_requested;
static int holder_error;
static int holder_drain_result;
static wasmfs_terminal_drain_result holder_drain_details;
#endif

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportResultOnBrowserThread(int role,
                                        int result,
                                        int error,
                                        int drain_result,
                                        int terminal_error,
                                        int data_file_states,
                                        int libc_flush_failed,
                                        int data_flush_failures,
                                        int data_close_failures,
                                        int backend_terminal_failures) {
  EM_ASM({
    window.parent.postMessage(
      {
        backendTerminalFailures: $9,
        dataCloseFailures: $8,
        dataFileStates: $5,
        dataFlushFailures: $7,
        drainResult: $3,
        error: $2,
        libcFlushFailed: $6,
        result: $1,
        role: $0,
        terminalError: $4,
        type: 'wasmfs-opfs-terminal-drain',
      },
      window.location.origin);
  }, role,
     result,
     error,
     drain_result,
     terminal_error,
     data_file_states,
     libc_flush_failed,
     data_flush_failures,
     data_close_failures,
     backend_terminal_failures);
}

static void ReportResult(int role,
                         int result,
                         int error,
                         int drain_result,
                         const wasmfs_terminal_drain_result* details) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIIIIIIIIII,
    ReportResultOnBrowserThread,
    role,
    result,
    error,
    drain_result,
    details->error,
    details->data_file_states,
    details->libc_flush_failed,
    details->data_flush_failures,
    details->data_close_failures,
    details->backend_terminal_failures);
}

static int MountLeasedOPFS(void) {
  errno = 0;
  backend_t backend = wasmfs_create_opfs_backend_with_profile_lease(kProfileName);
  if (!backend) {
    return ErrorOrEIO();
  }

#ifdef WASMFS_OPFS_TERMINAL_DRAIN_HOLDER
  // A terminal WasmFS instance has exactly one coordinated lease handoff. The
  // second factory must fail before it creates a ProxyWorker or requests a
  // second browser lease, even with a distinct valid profile name.
  errno = 0;
  if (wasmfs_create_opfs_backend_with_profile_lease(kSecondProfileName) !=
        NULL ||
      errno != EBUSY) {
    return EIO;
  }
#endif

  int result = wasmfs_create_directory("/opfs", 0777, backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

#ifdef WASMFS_OPFS_TERMINAL_DRAIN_HOLDER

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_terminal_drain_holder_shutdown(void) {
  atomic_store(&shutdown_requested, 1);
}

// Invoked directly by the holder iframe's browser JS main thread after the
// application pthread has completed terminal drain. It specifically checks
// the PROXY_TO_PTHREAD case where runtime-main and browser-main differ.
EMSCRIPTEN_KEEPALIVE
int wasmfs_opfs_terminal_drain_browser_main_attempt(void) {
  wasmfs_terminal_drain_result result = {0};
  // terminalDrain validates its calling context before it begins a result-
  // bearing cleanup attempt, so its zeroed output is intentionally unchanged.
  return wasmfs_terminal_drain(&result) == -EAGAIN ? 0 : 1;
}

static void ShutdownWhenRequested(void) {
  if (atomic_exchange(&shutdown_requested, 0)) {
    // The controller calls this only after it has tested a fresh iframe while
    // this holder remains live. A nonzero status is an extra teardown witness;
    // this test makes no assertion about browser-context crash recovery.
    exit(holder_error == 0 ? 0 : 1);
  }
}

#if defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN) || \
  defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
static int OpenWritableFile(void) {
  int fd = open(kFilePath, O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) {
    return -1;
  }
  if (write(fd, "x", 1) != 1) {
    int error = ErrorOrEIO();
    (void)close(fd);
    errno = error;
    return -1;
  }
  return fd;
}
#endif

static int CheckDrainResult(int drainResult,
                            const wasmfs_terminal_drain_result* result) {
#if defined(WASMFS_OPFS_TERMINAL_DRAIN_RETIRE_FAILURE)
  // The injected error is deliberately after Web Locks release, allocator
  // reset, and heartbeat stop. terminalDrain must preserve it as a structured
  // non-success while native retirement still makes EXIT_RUNTIME safe.
  return drainResult == -EIO && result->error == -EIO &&
         result->data_file_states == 3 &&
         result->libc_flush_failed == 0 &&
         result->data_flush_failures == 0 &&
         result->data_close_failures == 0 &&
         result->backend_terminal_failures == 1
           ? 0
           : EIO;
#elif defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN)
  // The injected close occurs while terminalDrain owns the descriptor table.
  // It must both report the data close failure and retain the still-live lease.
  return drainResult == -EIO && result->error == -EIO &&
         result->data_file_states == 4 &&
         result->libc_flush_failed == 0 &&
         result->data_flush_failures == 0 &&
         result->data_close_failures == 1 &&
         // The lease finalizer sees the earlier data-close error and retains
         // the lease without attempting a second failing finalization.
         result->backend_terminal_failures == 0
           ? 0
           : EIO;
#elif defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
  // __wasi_fd_close already removed the failed descriptor. The backend-level
  // latch is therefore the only terminal error and prevents lease release.
  return drainResult == -EIO && result->error == -EIO &&
         result->data_file_states == 3 &&
         result->libc_flush_failed == 0 &&
         result->data_flush_failures == 0 &&
         result->data_close_failures == 0 &&
         result->backend_terminal_failures == 1
           ? 0
           : EIO;
#else
  return drainResult == 0 && result->error == 0 &&
         result->data_file_states == 3 &&
         result->libc_flush_failed == 0 &&
         result->data_flush_failures == 0 &&
         result->data_close_failures == 0 &&
         result->backend_terminal_failures == 0
           ? 0
           : EIO;
#endif
}

#if !defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN) && \
  !defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
static void* ChurnPthreadAfterTerminalRetirement(void* arg) {
  (void)arg;
  // Terminal drain has already retired the dedicated OPFS worker. This
  // ordinary pthread must therefore exercise only the remaining pool, not a
  // worker whose OPFS globals/heartbeat were cleared.
  return NULL;
}

static int CreateCachedNestedDirectory(void) {
  if (mkdir("/opfs/nested", 0700) != 0) {
    if (errno != EEXIST) {
      return ErrorOrEIO();
    }
  }
  int fd = open("/opfs/nested/cached", O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0 || write(fd, "n", 1) != 1 || close(fd) != 0) {
    return ErrorOrEIO();
  }
  int directory = open("/opfs/nested", O_RDONLY | O_DIRECTORY);
  if (directory < 0 || close(directory) != 0) {
    return ErrorOrEIO();
  }
  // No descriptor survives, but the mounted OPFS tree retains these wrapper
  // objects until global WasmFS destruction after EXIT_RUNTIME.
  return 0;
}
#endif

static int RunHolder(void) {
  int error = MountLeasedOPFS();
  if (error != 0) {
    return error;
  }

#if !defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN) && \
  !defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
  if ((error = CreateCachedNestedDirectory()) != 0) {
    return error;
  }
#endif

#if defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN) || \
  defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
  int fd = OpenWritableFile();
  if (fd < 0) {
    return ErrorOrEIO();
  }
#if defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
  // The injected failure happens before browser close(). The regular close
  // removes its FileTable entry, so terminalDrain must find the backend latch
  // rather than releasing the profile lease from an apparently clean table.
  errno = 0;
  if (close(fd) == 0 || errno != EIO) {
    return ErrorOrEIO();
  }
#endif
#endif

  holder_drain_result = wasmfs_terminal_drain(&holder_drain_details);
  error = CheckDrainResult(holder_drain_result, &holder_drain_details);
#if !defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN) && \
  !defined(WASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN)
  if (error == 0) {
    pthread_t churn;
    if (pthread_create(&churn, NULL, ChurnPthreadAfterTerminalRetirement,
                       NULL) != 0 ||
        pthread_join(churn, NULL) != 0) {
      return EIO;
    }
  }
#endif
  return error;
}

#else

static void KeepRuntimeAlive(void) {}

static int RunContender(void) {
  return MountLeasedOPFS();
}

#endif

int main(void) {
#ifdef WASMFS_OPFS_TERMINAL_DRAIN_HOLDER
  holder_error = RunHolder();
  ReportResult(kHolder,
               holder_error == EBUSY ? kBusy
                                     : holder_error == 0 ? kReady
                                                         : kOtherFailure,
               holder_error,
               holder_drain_result,
               &holder_drain_details);
  // Keep the module and its worker context alive after terminal drain. The
  // parent starts a separate module before asking this holder to exit.
  emscripten_set_main_loop(ShutdownWhenRequested, 0, false);
#else
  int error = RunContender();
  wasmfs_terminal_drain_result no_drain_details = {0};
  ReportResult(kContender,
               error == EBUSY ? kBusy : error == 0 ? kReady : kOtherFailure,
               error,
               0,
               &no_drain_details);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#endif
}
