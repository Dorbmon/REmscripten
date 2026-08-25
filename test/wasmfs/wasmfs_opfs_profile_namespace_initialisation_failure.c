// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.

// This is a focused factory-lifecycle test, not a power-loss or Chromium
// profile proof. The linked WasmFS variation either discards a completed
// initialization acknowledgement (to exercise the native tombstone policy) or
// returns a known error just after creating a private bootstrap file. The
// browser harness keeps holders alive or disposes them deliberately, then uses
// an ordinary fresh module to prove the expected handoff/recovery outcome.

#include <errno.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_TOMBSTONE) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_KNOWN) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_FRESH)
#error "select an initialisation failure test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_TOMBSTONE) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_KNOWN) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_FRESH) != 1
#error "select exactly one initialisation failure test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_PROFILE_NAME
#error "select an initialisation failure profile name"
#endif

#define WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_STRINGIFY_IMPL(value)

enum TestRole {
  kTombstone,
  kKnownFailure,
  kFresh,
};

enum TestResult {
  kReady,
  kBusy,
  kTombstoned,
  kFailure,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_STRINGIFY(
    WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_PROFILE_NAME);

static int ErrorOrEIO(void) {
  return errno ? errno : EIO;
}

static void ReportResultOnBrowserThread(int role,
                                        int result,
                                        int error,
                                        int stage) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        stage: $3,
        type: 'wasmfs-opfs-profile-namespace-initialisation-failure',
      },
      window.location.origin);
  }, role, result, error, stage);
}

static void ReportResult(int role, int result, int error, int stage) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIIII, ReportResultOnBrowserThread, role, result, error, stage);
}

static int DrainScoped(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired
           ? 0
           : EIO;
}

static int CreateMountAndDrain(int* stage) {
  *stage = 1;
  errno = 0;
  backend_t backend = wasmfs_create_opfs_profile_namespace_backend(
    kProfileName);
  if (!backend) {
    return ErrorOrEIO();
  }
  *stage = 2;
  const int mount_result = wasmfs_create_directory("/profile", 0700, backend);
  if (mount_result != 0) {
    // WasmFS's public mount entry point returns its negative errno directly;
    // it is not required to set libc errno for this C ABI path. Preserve that
    // result so the browser witness diagnoses an actual failed mount instead
    // of an unrelated stale errno from an earlier operation.
    return mount_result < 0 ? -mount_result : EIO;
  }
  // A namespace backend deliberately has one mount identity. A second mount
  // must reject rather than manufacture a wrapper with independent append or
  // record-lock state for the same logical root.
  *stage = 3;
  errno = 0;
  const int alias_result =
      wasmfs_create_directory("/profile-alias", 0700, backend);
  if (alias_result != -EIO) {
    // Preserve the unexpected raw result in the bounded browser diagnostic.
    // The normal test stages are single digits, so this cannot be confused
    // with a normal failure boundary.
    *stage = 3000 + alias_result;
    return alias_result < 0 ? -alias_result : EIO;
  }
  *stage = 4;
  return DrainScoped(backend);
}

static int VerifyTombstone(void) {
  errno = 0;
  backend_t backend = wasmfs_create_opfs_profile_namespace_backend(
    kProfileName);
  if (backend || ErrorOrEIO() != EIO) {
    return EIO;
  }

  // The failed factory is absent from backendTable, but its reservation must
  // remain visible to terminal drain. A global terminal success here would
  // falsely claim that a detached worker/possible Web Lock was handed off.
  errno = 0;
  if (wasmfs_create_opfs_profile_namespace_backend(kProfileName) ||
      ErrorOrEIO() != EBUSY) {
    return EIO;
  }
  wasmfs_terminal_drain_result terminal = {0};
  int result = wasmfs_terminal_drain(&terminal);
  return result == -ESHUTDOWN && terminal.error == -ESHUTDOWN &&
         terminal.backend_terminal_failures == 1
           ? 0
           : EIO;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  int role;
  int error;
  int stage = 0;
#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_TOMBSTONE)
  role = kTombstone;
  error = VerifyTombstone();
#elif defined(WASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_KNOWN)
  role = kKnownFailure;
  errno = 0;
  backend_t backend = wasmfs_create_opfs_profile_namespace_backend(
    kProfileName);
  error = backend || ErrorOrEIO() != EIO ? EIO : 0;
#else
  role = kFresh;
  error = CreateMountAndDrain(&stage);
#endif
  ReportResult(role,
               error == EBUSY ? kBusy :
                 error == 0 && role == kTombstone ? kTombstoned :
                 error == 0 ? kReady : kFailure,
               error,
               stage);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
