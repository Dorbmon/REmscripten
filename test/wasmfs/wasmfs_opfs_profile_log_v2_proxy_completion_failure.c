// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// This regression synthetically enters the lost-native-acknowledgement state
// after the V2 control primitive has successfully written and flushed its
// inactive root image. It checks that the holder does not release its lease,
// that the explicit failed drain submits no later Worker proxy, that another
// fresh document remains blocked while the holder is live, and that a later
// fresh document reads the old selected root after iframe disposal. It cannot
// observe C++ destruction after disposal and is not a browser-crash, literal
// ProxyWorker-failure, or physical OPFS directory-durability claim.

#include <errno.h>
#include <stdint.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_HOLDER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_CONTENDER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_VERIFIER)
#error "select one V2 proxy completion failure test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_HOLDER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_CONTENDER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_VERIFIER) != 1
#error "select exactly one V2 proxy completion failure test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_PROXY_PROFILE_NAME
#error "select a V2 proxy completion failure profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V2_PROXY_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V2_PROXY_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V2_PROXY_STRINGIFY_IMPL(value)

enum TestRole {
  kHolder,
  kContender,
  kVerifier,
};

enum TestResult {
  kTerminalFailure,
  kBusy,
  kRecoveredOldRoot,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V2_PROXY_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V2_PROXY_PROFILE_NAME);

#if defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_HOLDER)
extern int wasmfs_opfs_profile_log_v2_test_proxies_after_latch(void);
#endif

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v2-proxy-completion',
      },
      window.location.origin);
  }, role, result, error);
}

static void Report(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, result, error);
}

static int DrainSuccess(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0
           ? 0
           : EIO;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;
  int result = kTerminalFailure;

#if defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_HOLDER)
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v2_control_backend(kProfileName);
  if (!backend) {
    error = ErrorOrEIO();
  }
  if (error == 0 && wasmfs_opfs_profile_log_v2_commit_root(
                      backend, UINT64_C(0x706f69736f6e)) != -EIO) {
    error = EIO;
  }
  if (backend) {
    wasmfs_opfs_profile_drain_result details = {0};
    const int drain = wasmfs_drain_opfs_profile_backend(backend, &details);
    if (drain != -EIO || details.error != -EIO || !details.backend_sealed ||
        details.lease_released || details.backend_retired) {
      error = EIO;
    }
  }
  if (wasmfs_opfs_profile_log_v2_test_proxies_after_latch() != 0) {
    error = EIO;
  }
  result = kTerminalFailure;
  Report(kHolder, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V2_PROXY_CONTENDER)
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v2_control_backend(kProfileName);
  if (backend || errno != EBUSY) {
    error = EIO;
  }
  if (backend) {
    const int drainError = DrainSuccess(backend);
    if (drainError != 0) {
      error = drainError;
    }
  }
  result = kBusy;
  Report(kContender, result, error);
#else
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v2_control_backend(kProfileName);
  if (!backend) {
    error = ErrorOrEIO();
  }
  uint64_t root = UINT64_MAX;
  if (error == 0 &&
      (wasmfs_opfs_profile_log_v2_read_root(backend, &root) != 0 ||
       root != 0)) {
    error = EIO;
  }
  if (backend) {
    const int drainError = DrainSuccess(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  result = kRecoveredOldRoot;
  Report(kVerifier, result, error);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
