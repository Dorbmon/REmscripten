/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  See the LICENSE file for
 * details.
 */

// Fresh-document proof for the explicit fail-closed profile-retirement API.
// The holder uses the V4 filesystem so its live bootstrap, control, and arena
// OPFSFile helpers are open when higher-level profile work rejects a clean
// handoff. The failure-retirement path must close those helpers before raw
// runtime destruction, retain the cooperative Web Lock while the holder stays
// live, and leave a later fresh document able to recover the already-synced
// marker and perform an ordinary clean drain. This is a teardown-safety proof,
// not a claim that failed higher-level profile work has a durable handoff.
//
// The separate V4 proxy-completion role family below reuses this holder /
// contender / actual-EXIT_RUNTIME harness. It starts from an independently
// drained seed, then faults after a real V4 manifest flush but before outer
// V4 publication. It is a controlled acknowledgement-loss test, not a
// literal ProxyWorker failure, browser crash, power-loss, or OPFS directory
// durability simulation.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <emscripten/wasmfs_opfs_profile_drain.h>

#if !defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER) && \
  !defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_CONTENDER) && \
  !defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_CONTENDER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_RELOAD)
#error "select one profile fail-closed or V4 proxy-completion test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER) + \
      defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_CONTENDER) + \
      defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_CONTENDER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_RELOAD) != 1
#error "select exactly one profile fail-closed or V4 proxy-completion test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_SEED) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_CONTENDER) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_VERIFIER) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_RELOAD)
#define WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST 1
#else
#define WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST 0
#endif

#ifndef WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_PROFILE_NAME
#error "select a fail-closed profile-retirement test profile name"
#endif

#define WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_STRINGIFY_IMPL(value)

enum TestRole {
  kHolder,
  kContender,
  kVerifier,
  kProxySeed,
  kProxyHolder,
  kProxyContender,
  kProxyVerifier,
  kProxyReload,
};

enum TestResult {
  kReady,
  kBusy,
  kFailure,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_STRINGIFY(
    WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-fail-closed-retirement";
static const char kMarkerPath[] =
  "/v4fs-fail-closed-retirement/marker";
static const uint8_t kMarker[] = {
  0x66, 0x61, 0x69, 0x6c, 0x2d, 0x63, 0x6c, 0x6f,
  0x73, 0x65, 0x64, 0x2d, 0x76, 0x34,
};
static const char kProxyMountPath[] = "/v4fs-proxy-completion";
static const char kProxyMarkerPath[] = "/v4fs-proxy-completion/marker";
static const char kProxyReplacementPath[] =
  "/v4fs-proxy-completion/replacement";
static const char kProxyThreadScratchPath[] =
  "/v4fs-proxy-completion/thread-affinity";
static const uint8_t kProxySeedMarker[] = "V4-proxy-completion-A";
static const uint8_t kProxyRejectedMarker[] = "V4-proxy-completion-B";
static const uint8_t kProxyRecoveredMarker[] = "V4-proxy-completion-C";
static const uint8_t kProxyThreadScratchMarker[] =
  "V4-proxy-completion-thread";

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int result, int error) {
#if WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-proxy-completion',
      },
      window.location.origin);
  }, role, result, error);
#else
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-fail-closed-retirement',
      },
      window.location.origin);
  }, role, result, error);
#endif
}

static void Report(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, result, error);
}

static int MountFilesystem(backend_t* backend, const char* mount_path) {
  if (!backend || !mount_path) {
    return EINVAL;
  }
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(mount_path, 0700, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainFilesystem(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static int RunHolder(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend, kMountPath);
  if (error) {
    return error;
  }

  const int marker = open(kMarkerPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (marker < 0) {
    return ErrorOrEIO();
  }
  if (write(marker, kMarker, sizeof(kMarker)) != (ssize_t)sizeof(kMarker) ||
      fdatasync(marker) != 0) {
    return ErrorOrEIO();
  }

  wasmfs_opfs_profile_drain_result details = {0};
  const int result =
    wasmfs_fail_closed_opfs_profile_backend(backend, &details);
  if (result != -ESHUTDOWN || details.error != -ESHUTDOWN ||
      details.detached_descriptors != 1 || details.data_file_states != 1 ||
      details.libc_flush_failed != 0 || details.data_flush_failures != 0 ||
      details.data_close_failures != 0 || details.prior_close_failures != 0 ||
      details.lease_release_failures != 0 ||
      details.backend_retire_failures != 0 || !details.backend_sealed ||
      details.lease_released || details.backend_retired) {
    return EIO;
  }

  // The failure path still detaches the descriptor table. It must also seal
  // the backend before the holder reports its retained-lock witness.
  char byte = 0;
  errno = 0;
  if (read(marker, &byte, 1) != -1 || errno != EBADF) {
    return EIO;
  }
  errno = 0;
  if (open(kMarkerPath, O_RDONLY) != -1 || errno != ESHUTDOWN) {
    return EIO;
  }

  wasmfs_opfs_profile_drain_result again = {0};
  return wasmfs_drain_opfs_profile_backend(backend, &again) == -ESHUTDOWN &&
         again.error == -ESHUTDOWN
           ? 0
           : EIO;
}

static int RunContender(void) {
  errno = 0;
  backend_t backend =
    wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (backend || errno != EBUSY) {
    return EIO;
  }
  return EBUSY;
}

static int RunVerifier(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend, kMountPath);
  if (error) {
    return error;
  }

  const int marker = open(kMarkerPath, O_RDONLY);
  if (marker < 0) {
    error = ErrorOrEIO();
  } else {
    uint8_t actual[sizeof(kMarker)] = {};
    if (read(marker, actual, sizeof(actual)) != (ssize_t)sizeof(actual) ||
        memcmp(actual, kMarker, sizeof(kMarker)) != 0) {
      error = ErrorOrEIO();
    }
    if (close(marker) != 0 && !error) {
      error = ErrorOrEIO();
    }
  }

  const int drain_error = DrainFilesystem(backend);
  return error ? error : drain_error;
}

static int WriteProxyMarkerAtPath(const char* path,
                                  const uint8_t* marker,
                                  size_t size,
                                  int create) {
  if (!path || !marker || !size) {
    return EINVAL;
  }
  const int flags = create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR;
  const int fd = open(path, flags, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = 0;
  if (pwrite(fd, marker, size, 0) != (ssize_t)size) {
    error = ErrorOrEIO();
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  if (close(fd) != 0 && !error) {
    error = ErrorOrEIO();
  }
  return error;
}

static int WriteProxyMarker(const uint8_t* marker, size_t size, int create) {
  return WriteProxyMarkerAtPath(kProxyMarkerPath, marker, size, create);
}

static int ReadProxyMarker(const uint8_t* marker, size_t size) {
  if (!marker || !size) {
    return EINVAL;
  }
  const int fd = open(kProxyMarkerPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  uint8_t actual[sizeof(kProxyRecoveredMarker)] = {};
  int error = 0;
  if (size > sizeof(actual) ||
      read(fd, actual, size) != (ssize_t)size ||
      memcmp(actual, marker, size) != 0) {
    error = ErrorOrEIO();
  }
  if (close(fd) != 0 && !error) {
    error = ErrorOrEIO();
  }
  return error;
}

static int RunProxySeed(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend, kProxyMountPath);
  if (!error) {
    error = WriteProxyMarker(
      kProxySeedMarker, sizeof(kProxySeedMarker), true);
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  return error;
}

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER)
struct ProxyThreadAffinityState {
  pthread_barrier_t start_barrier;
  int scratch;
  int error;
};

static int WaitForProxyThreadBarrier(pthread_barrier_t* barrier) {
  const int result = pthread_barrier_wait(barrier);
  return result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD ? 0 : EIO;
}

static void* MutateProxyThreadScratch(void* opaque) {
  struct ProxyThreadAffinityState* state = opaque;
  int error = WaitForProxyThreadBarrier(&state->start_barrier);
  if (!error &&
      pwrite(state->scratch,
             kProxyThreadScratchMarker,
             sizeof(kProxyThreadScratchMarker),
             0) != (ssize_t)sizeof(kProxyThreadScratchMarker)) {
    error = ErrorOrEIO();
  }
  if (!error && fdatasync(state->scratch) != 0) {
    error = ErrorOrEIO();
  }
  if (state->scratch >= 0) {
    if (close(state->scratch) != 0 && !error) {
      error = ErrorOrEIO();
    }
    state->scratch = -1;
  }
  state->error = error;
  return NULL;
}

static int RunProxyThreadAffinityProbe(int scratch) {
  struct ProxyThreadAffinityState state = {
    .scratch = scratch,
    .error = 0,
  };
  if (pthread_barrier_init(&state.start_barrier, NULL, 2) != 0) {
    close(scratch);
    return EIO;
  }

  pthread_t thread;
  int error = 0;
  if (pthread_create(&thread, NULL, MutateProxyThreadScratch, &state) != 0) {
    error = EIO;
    close(scratch);
    state.scratch = -1;
  } else {
    // The parent owns the one global arm. The barrier makes the child's real
    // V4 write, flush, and close happen after that arm but before the parent's
    // rejected B rename. A non-thread-affine seam would be consumed by the
    // child, which this test rejects before it reaches that replacement.
    if (wasmfs_opfs_profile_log_v4_test_proxy_completion_arm() != 1) {
      error = EIO;
    }
    if (WaitForProxyThreadBarrier(&state.start_barrier) != 0 && !error) {
      error = EIO;
    }
    if (pthread_join(thread, NULL) != 0 && !error) {
      error = EIO;
    }
    if (state.error && !error) {
      error = state.error;
    }
  }

  if (pthread_barrier_destroy(&state.start_barrier) != 0 && !error) {
    error = EIO;
  }
  return error;
}

static int RunProxyHolder(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend, kProxyMountPath);
  if (!error) {
    // Match ImportantFileWriter's final replacement boundary: B is written,
    // flushed, and closed under a temporary path before the selected parent
    // thread arms the V4 manifest publication fault and calls rename.
    error = WriteProxyMarkerAtPath(kProxyReplacementPath,
                                   kProxyRejectedMarker,
                                   sizeof(kProxyRejectedMarker),
                                   true);
  }
  if (!error) {
    const int scratch =
      open(kProxyThreadScratchPath, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (scratch < 0) {
      error = ErrorOrEIO();
    } else {
      error = RunProxyThreadAffinityProbe(scratch);
    }
  }
  if (!error) {
    errno = 0;
    const int rename_result = rename(kProxyReplacementPath, kProxyMarkerPath);
    const int rename_error = errno;
    if (rename_result != -1 || rename_error != EIO ||
        wasmfs_opfs_profile_log_v4_test_proxy_completion_arm() != 0) {
      error = EIO;
    }
  }
  if (backend) {
    wasmfs_opfs_profile_drain_result details = {0};
    // Chromium uses the explicit retained-lease failure disposition after its
    // real replacement error. There are no open user descriptors here: the
    // temporary B file and the thread-affinity scratch file were both closed
    // before the selected rename. The control must therefore prove that this
    // known post-flush seam seals/retains without a later proxy or a synthetic
    // cleanup failure.
    const int drain =
      wasmfs_fail_closed_opfs_profile_backend(backend, &details);
    if (!error &&
        (drain != -ESHUTDOWN || details.error != -ESHUTDOWN ||
         details.detached_descriptors != 0 || details.data_file_states != 0 ||
         details.libc_flush_failed != 0 || details.data_flush_failures != 0 ||
         details.data_close_failures != 0 || details.prior_close_failures != 0 ||
         details.lease_release_failures != 0 ||
         details.backend_retire_failures != 0 || !details.backend_sealed ||
         details.lease_released || details.backend_retired)) {
      error = EIO;
    }
    if (!error) {
      wasmfs_opfs_profile_drain_result again = {0};
      if (wasmfs_drain_opfs_profile_backend(backend, &again) != -ESHUTDOWN ||
          again.error != -ESHUTDOWN) {
        error = EIO;
      }
    }
  }
  // A zero post-latch proxy count proves the failed drain stayed quiescent
  // only if this holder also proves it actually crossed the post-flush seam.
  if (!error &&
      (wasmfs_opfs_profile_log_v4_test_proxy_completion_latch_count() != 1 ||
       wasmfs_opfs_profile_log_v4_test_proxies_after_latch() != 0)) {
    error = EIO;
  }
  return error;
}
#endif

static int RunProxyContender(void) {
  errno = 0;
  backend_t backend =
    wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (backend || errno != EBUSY) {
    return EIO;
  }
  return EBUSY;
}

static int RunProxyVerifier(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend, kProxyMountPath);
  if (!error) {
    error = ReadProxyMarker(kProxySeedMarker, sizeof(kProxySeedMarker));
  }
  // This first fresh logical write forces V4 to trim the unreachable,
  // post-flush replacement manifest before it can publish C.
  if (!error) {
    error = WriteProxyMarker(
      kProxyRecoveredMarker, sizeof(kProxyRecoveredMarker), false);
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  return error;
}

static int RunProxyReload(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend, kProxyMountPath);
  if (!error) {
    error = ReadProxyMarker(
      kProxyRecoveredMarker, sizeof(kProxyRecoveredMarker));
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  return error;
}

#if defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER)
static _Atomic int holderShutdownRequested;

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_profile_fail_closed_retirement_holder_shutdown(void) {
  atomic_store(&holderShutdownRequested, 1);
}

static void ExitHolderWhenRequested(void) {
  if (atomic_exchange(&holderShutdownRequested, 0)) {
    exit(0);
  }
}
#else
static void KeepRuntimeAlive(void) {}
#endif

int main(void) {
  assert(!emscripten_is_main_runtime_thread());
  int error = 0;

#if defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER)
  error = RunHolder();
  Report(kHolder, error == 0 ? kReady : kFailure, error);
  emscripten_set_main_loop(ExitHolderWhenRequested, 0, false);
#elif defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_CONTENDER)
  error = RunContender();
  Report(kContender,
         error == EBUSY ? kBusy : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#elif defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_VERIFIER)
  error = RunVerifier();
  Report(kVerifier,
         error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_SEED)
  error = RunProxySeed();
  Report(kProxySeed,
         error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER)
  error = RunProxyHolder();
  Report(kProxyHolder, error == 0 ? kReady : kFailure, error);
  emscripten_set_main_loop(ExitHolderWhenRequested, 0, false);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_CONTENDER)
  error = RunProxyContender();
  Report(kProxyContender,
         error == EBUSY ? kBusy : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_VERIFIER)
  error = RunProxyVerifier();
  Report(kProxyVerifier,
         error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#else
  error = RunProxyReload();
  Report(kProxyReload,
         error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#endif
}
