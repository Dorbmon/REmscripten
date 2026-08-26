// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document crash-boundary proof for the V4 mountable profile filesystem.
// The parent disposes an iframe at a native bootstrap checkpoint, then retries
// from a new document after the browser has released the worker-owned lease.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_FAIL_CLOSED)
#error "select one V4 filesystem recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_FAIL_CLOSED) != 1
#error "select exactly one V4 filesystem recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_PROFILE_NAME
#error "select a V4 filesystem recovery test profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_STRINGIFY_IMPL(value) \
  #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_STRINGIFY_IMPL(value)

enum TestRole {
  kInterruptor,
  kVerifier,
  kReload,
  kFailClosed,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-bootstrap-recovery";
static const char kWitnessPath[] = "/v4fs-bootstrap-recovery/recovered";
static const uint8_t kWitness[] = {
  0x72, 0x65, 0x63, 0x6f, 0x76, 0x65, 0x72, 0x65, 0x64,
};

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-recovery',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int checkpoint) {
  EM_ASM({
    window.parent.postMessage(
      {
        checkpoint: $0,
        event: 'interrupt',
        type: 'wasmfs-opfs-profile-log-v4-filesystem-recovery',
      },
      window.location.origin);
  }, checkpoint);
}
#endif

// This private link symbol exists only in a test build selected with
// -sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1. Production builds expose no
// host mutation route and never link this hook.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPT_PHASE
  if (checkpoint !=
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPT_PHASE) {
    return;
  }
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VI, ReportInterruptionOnBrowserThread, checkpoint);
  while (1) {
    emscripten_thread_sleep(1000);
  }
#else
  (void)checkpoint;
#endif
}

static int MountFilesystem(backend_t* backend) {
  if (!backend) {
    return EINVAL;
  }
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(kMountPath, 0700, *backend);
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

static int WriteAndReadWitness(void) {
  const int fd = open(kWitnessPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = 0;
  if (pwrite(fd, kWitness, sizeof(kWitness), 0) != (ssize_t)sizeof(kWitness)) {
    error = ErrorOrEIO();
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  uint8_t actual[sizeof(kWitness)] = {};
  if (!error && pread(fd, actual, sizeof(actual), 0) != (ssize_t)sizeof(actual)) {
    error = ErrorOrEIO();
  }
  if (!error && memcmp(actual, kWitness, sizeof(actual)) != 0) {
    error = EIO;
  }
  if (close(fd) != 0 && !error) {
    error = ErrorOrEIO();
  }
  if (error) {
    return error;
  }
  const int directory = open(kMountPath, O_RDONLY | O_DIRECTORY);
  if (directory < 0) {
    return ErrorOrEIO();
  }
  error = fsync(directory) == 0 ? 0 : ErrorOrEIO();
  if (close(directory) != 0 && !error) {
    error = ErrorOrEIO();
  }
  return error;
}

static int VerifyAndRewriteExistingWitness(void) {
  const int fd = open(kWitnessPath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  uint8_t actual[sizeof(kWitness)] = {};
  int error = 0;
  if (pread(fd, actual, sizeof(actual), 0) != (ssize_t)sizeof(actual)) {
    error = ErrorOrEIO();
  }
  if (!error && memcmp(actual, kWitness, sizeof(actual)) != 0) {
    error = EIO;
  }
  if (!error &&
      pwrite(fd, kWitness, sizeof(kWitness), 0) != (ssize_t)sizeof(kWitness)) {
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

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR)
  error = MountFilesystem(&backend);
  // A selected interruption checkpoint does not return. If it does, the
  // factory failed before reaching the requested native transition.
  if (!error) {
    error = EIO;
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(kInterruptor, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_VERIFIER)
  error = MountFilesystem(&backend);
  if (!error) {
    error = WriteAndReadWitness();
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(kVerifier, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_RELOAD)
  error = MountFilesystem(&backend);
  if (!error) {
    error = VerifyAndRewriteExistingWitness();
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(kReload, error);
#else
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!backend && errno == EBUSY) {
    Report(kFailClosed, EBUSY);
  } else {
    error = backend || errno != EIO ? EIO : 0;
    if (backend) {
      const int drain_error = DrainFilesystem(backend);
      if (!error) {
        error = drain_error;
      }
    }
    Report(kFailClosed, error);
  }
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
