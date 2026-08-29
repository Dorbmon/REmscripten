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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER) && \
  !defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_CONTENDER) && \
  !defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_VERIFIER)
#error "select one fail-closed profile-retirement test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER) + \
      defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_CONTENDER) + \
      defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_VERIFIER) != 1
#error "select exactly one fail-closed profile-retirement test role"
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

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int result, int error) {
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
}

static void Report(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, result, error);
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

static int RunHolder(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend);
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
  int error = MountFilesystem(&backend);
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

#if defined(WASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER)
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
#else
  error = RunVerifier();
  Report(kVerifier,
         error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
         error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#endif
}
