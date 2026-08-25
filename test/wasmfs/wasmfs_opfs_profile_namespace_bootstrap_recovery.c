// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This browser-coordinated regression stops a first namespace mount at each
// durable bootstrap boundary, destroys that iframe, and lets a fresh document
// mount the same profile. It proves the protocol's intentionally narrow
// PREPARED/PUBLISHED recovery rule, including the first root-mode decision.
// It does not model a browser crash, power loss, SQLite, LevelDB, or a complete
// Chromium profile.

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INTERRUPTER) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_VERIFIER)
#error "select an OPFS profile namespace bootstrap recovery role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INTERRUPTER) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_VERIFIER) != 1
#error "select exactly one OPFS profile namespace bootstrap recovery role"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PROFILE_NAME
#error "select an OPFS profile namespace bootstrap profile name"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INITIAL_MODE
#error "select the first mount mode"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INTERRUPTER) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PHASE)
#error "select an OPFS profile namespace bootstrap interruption phase"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_EXPECTED_MODE)
#error "select the recovered root mode"
#endif

#define WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_STRINGIFY_IMPL(value)

enum TestRole {
  kInterrupter,
  kVerifier,
};

enum TestEvent {
  kResult,
  kInterruption,
};

enum TestResult {
  kReady,
  kBusy,
  kFailure,
};

enum BootstrapPhase {
  // The first tree payload is durable but no selector identifies it.
  kBeforeInitialSelector = 1,
  // The initial selector is durable but the bootstrap journal is PREPARED.
  kAfterInitialSelector = 2,
  // The PREPARED journal record is durable before any canonical file exists.
  kAfterPreparedJournal = 3,
  // The PUBLISHED journal record is durable before the root is exposed.
  kAfterPublishedJournal = 4,
  // Both PUBLISHED copies are durable before the root is exposed.
  kAfterPublishedJournalMirror = 5,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_STRINGIFY(
    WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PROFILE_NAME);
static const char kMountPath[] = "/profile";
static const mode_t kInitialMode =
  WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INITIAL_MODE;

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role,
                                  int event,
                                  int result,
                                  int error,
                                  int mode) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $3,
        event: $1,
        mode: $4,
        role: $0,
        result: $2,
        type: 'wasmfs-opfs-profile-namespace-bootstrap-recovery',
      },
      window.location.origin);
  }, role, event, result, error, mode);
}

static void Report(int role, int event, int result, int error, int mode) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIIIII,
    ReportOnBrowserThread,
    role,
    event,
    result,
    error,
    mode);
}

static int CreateAndMount(backend_t* backend, mode_t mode) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_namespace_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(kMountPath, mode, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainNamespace(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired
           ? 0
           : EIO;
}

static void KeepRuntimeAlive(void) {}

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INTERRUPTER)
static _Atomic int interruptionReported;

// This test bridge is linked only into the interrupter module. The matching
// libwasmfs variation calls it after the documented durable boundary; it never
// returns, so destroying the iframe is the only release path under test.
void wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(int phase) {
  if (phase != WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PHASE ||
      atomic_exchange(&interruptionReported, 1)) {
    return;
  }
  Report(kInterrupter, kInterruption, kReady, 0, phase);
  while (1) {
    emscripten_thread_sleep(1000);
  }
}
#endif

int main(void) {
  backend_t backend = NULL;
  int error = 0;
  int observedMode = -1;

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INTERRUPTER)
  error = CreateAndMount(&backend, kInitialMode);
  // Every passing interrupter blocks in the test bridge before this point.
  // Reporting a result means the configured protocol boundary was skipped.
  if (error == 0) {
    error = EIO;
  }
  Report(kInterrupter, kResult, kFailure, error, observedMode);
#else
  const mode_t expectedMode =
    WASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_EXPECTED_MODE;
  // Always request the retry mode, rather than the expected persisted mode.
  // The post-first-PUBLISHED cases must prove that their original 0711 mode
  // survives a fresh caller which asks for 0750.
  error = CreateAndMount(&backend, kInitialMode);
  if (error == 0) {
    struct stat status = {};
    if (stat(kMountPath, &status) != 0) {
      error = ErrorOrEIO();
    } else {
      observedMode = status.st_mode & (S_IRWXUGO | S_ISVTX);
      if (observedMode != expectedMode) {
        error = EIO;
      }
    }
  }
  if (backend) {
    const int drainError = DrainNamespace(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kVerifier,
         kResult,
         error == EBUSY ? kBusy : error == 0 ? kReady : kFailure,
         error,
         observedMode);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
