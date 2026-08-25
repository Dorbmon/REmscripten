// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.

// This browser-coordinated test covers the namespace factory's unmounted
// lifecycle corner. A factory durably writes only a private PREPARED journal
// before a caller mounts its logical root. A successful scoped or terminal
// drain must release the lease while retaining that durable unexposed state,
// and permit a still-live fresh document to mount and publish the same profile.

#include <errno.h>
#include <stdio.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_DRAIN) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_FRESH)
#error "select an empty namespace drain test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_DRAIN) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_FRESH) != 1
#error "select exactly one empty namespace drain test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_TERMINAL) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_DRAIN)
#error "terminal empty namespace drain must use the drain role"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_PROFILE_NAME
#error "select an empty namespace drain profile name"
#endif

#define WASMFS_OPFS_PROFILE_NAMESPACE_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_NAMESPACE_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_NAMESPACE_STRINGIFY_IMPL(value)

enum TestRole {
  kDrain,
  kFresh,
};

enum TestResult {
  kReady,
  kBusy,
  kFailure,
};

static int ErrorOrEIO(void) {
  return errno ? errno : EIO;
}

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_NAMESPACE_STRINGIFY(
    WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_PROFILE_NAME);

static void ReportResultOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-namespace-empty-drain',
      },
      window.location.origin);
  }, role, result, error);
}

static void ReportResult(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportResultOnBrowserThread, role, result, error);
}

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_TERMINAL) || \
  defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_FRESH)
static int DrainScoped(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  if (result != 0 || details.error != 0 || !details.backend_sealed ||
      !details.lease_released || !details.backend_retired) {
    return EIO;
  }
  return 0;
}
#endif

#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_TERMINAL
static int DrainTerminal(void) {
  wasmfs_terminal_drain_result details = {0};
  int result = wasmfs_terminal_drain(&details);
  if (result != 0 || details.error != 0 || details.data_file_states != 3 ||
      details.libc_flush_failed != 0 || details.data_flush_failures != 0 ||
      details.data_close_failures != 0 ||
      details.backend_terminal_failures != 0) {
    return EIO;
  }
  return 0;
}
#endif

static void KeepRuntimeAlive(void) {}

int main(void) {
  errno = 0;
  backend_t backend = wasmfs_create_opfs_profile_namespace_backend(
    kProfileName);
  int error = backend ? 0 : ErrorOrEIO();

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_DRAIN)
  const int role = kDrain;
  if (error == 0) {
#ifdef WASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_TERMINAL
    error = DrainTerminal();
#else
    error = DrainScoped(backend);
#endif
  }
#else
  const int role = kFresh;
  if (error == 0 &&
      wasmfs_create_directory("/profile", 0700, backend) != 0) {
    error = ErrorOrEIO();
  }
  if (backend) {
    int drain_error = DrainScoped(backend);
    if (error == 0) {
      error = drain_error;
    }
  }
#endif

  ReportResult(role,
               error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
               error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
