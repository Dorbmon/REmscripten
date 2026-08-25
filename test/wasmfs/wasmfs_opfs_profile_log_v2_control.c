// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Browser-coordinated proof for the deliberately small V2 profile-log control
// primitive. It writes no host JavaScript OPFS state. A test-only factory or
// mutator hook is stopped by disposing its iframe after the first bootstrap
// witness or after one or both native CLEAN witnesses have flushed. A fresh
// iframe/module then checks the allowed recovery outcome. This is neither a
// browser-crash nor a power-loss test, and it does not claim directory,
// SQLite, LevelDB, or Chromium profile recovery.

#include <errno.h>
#include <stdint.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_MUTATOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_CORRUPTOR)
#error "select one V2 profile-log control test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_MUTATOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_CORRUPTOR) != 1
#error "select exactly one V2 profile-log control test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROFILE_NAME
#error "select a V2 profile-log control test profile name"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT
#define WASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT UINT64_C(0)
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V2_TEST_NEW_ROOT
#define WASMFS_OPFS_PROFILE_LOG_V2_TEST_NEW_ROOT UINT64_C(0x4c6f675632)
#endif

#define WASMFS_OPFS_PROFILE_LOG_V2_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V2_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V2_STRINGIFY_IMPL(value)

enum TestRole {
  kOwner,
  kMutator,
  kVerifier,
  kCorruptor,
};

enum TestResult {
  kReady,
  kCorruptionRejected,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V2_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V2_TEST_PROFILE_NAME);

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v2-control',
      },
      window.location.origin);
  }, role, result, error);
}

static void Report(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, result, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int phase) {
  EM_ASM({
    window.parent.postMessage(
      {
        event: 'interrupt',
        phase: $0,
        type: 'wasmfs-opfs-profile-log-v2-control',
      },
      window.location.origin);
  }, phase);
}
#endif

// The V2 library references this test-only symbol only in its matching
// interrupt variation. Defining a no-op in the normal role binaries keeps the
// test source uniform without exposing a production hook.
void wasmfs_opfs_profile_log_v2_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT_PHASE
  if (checkpoint != WASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT_PHASE) {
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

static int OpenControl(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v2_control_backend(kProfileName);
  return *backend ? 0 : ErrorOrEIO();
}

static int ReadExpectedRoot(backend_t backend, uint64_t expected) {
  uint64_t actual = 0;
  const int result = wasmfs_opfs_profile_log_v2_read_root(backend, &actual);
  return result == 0 && actual == expected ? 0 : result < 0 ? -result : EIO;
}

static int DrainControl(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static int CheckControlBoundary(backend_t control) {
  // V2's control-only factory must never be usable as a mount provider. The
  // generic mount C ABI reports factory refusal as -EIO.
  if (wasmfs_create_directory("/v2-control-not-mountable", 0700, control) !=
      -EIO) {
    return EIO;
  }

  // An owned ordinary backend is valid at the C ABI boundary but cannot be
  // dispatched as a V2 control store. These calls must reject explicitly,
  // rather than treating arbitrary persistent storage as the V2 protocol.
  backend_t ordinary = wasmfs_create_memory_backend();
  uint64_t value = 0;
  if (!ordinary ||
      wasmfs_opfs_profile_log_v2_read_root(ordinary, &value) != -ENOTSUP ||
      wasmfs_opfs_profile_log_v2_commit_root(ordinary, UINT64_C(1)) !=
        -ENOTSUP) {
    return EIO;
  }
  return 0;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;
  int result = kReady;

#if defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER)
  error = OpenControl(&backend);
  if (error == 0) {
    error = CheckControlBoundary(backend);
  }
  if (error == 0) {
    error = ReadExpectedRoot(backend, UINT64_C(0));
  }
  if (backend) {
    const int drainError = DrainControl(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kOwner, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_MUTATOR)
  error = OpenControl(&backend);
  if (error == 0) {
    error = ReadExpectedRoot(backend, UINT64_C(0));
  }
  if (error == 0) {
    const int commit = wasmfs_opfs_profile_log_v2_commit_root(
      backend, WASMFS_OPFS_PROFILE_LOG_V2_TEST_NEW_ROOT);
    error = commit < 0 ? -commit : commit;
  }
  if (backend) {
    const int drainError = DrainControl(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kMutator, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V2_TEST_VERIFIER)
  error = OpenControl(&backend);
  if (error == 0) {
    error = ReadExpectedRoot(
      backend, WASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT);
  }
#ifdef WASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECT_COMMIT_REJECTION
  if (error == 0 && wasmfs_opfs_profile_log_v2_commit_root(
                      backend, WASMFS_OPFS_PROFILE_LOG_V2_TEST_NEW_ROOT) !=
                    -ESHUTDOWN) {
    error = EIO;
  }
  // The rejected update must not overwrite the inactive g+1 descriptor/root
  // chain from the interrupted transaction. Re-read before the verifier
  // drains to make that read-only guarantee observable.
  if (error == 0) {
    error = ReadExpectedRoot(
      backend, WASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT);
  }
#endif
  if (backend) {
    const int drainError = DrainControl(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kVerifier, result, error);
#else
  // A selected-control parser fault must fail the factory before it produces a
  // usable backend. The factory's own failed-initialisation path performs the
  // leased worker handoff; a surprise backend is drained only for test hygiene.
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v2_control_backend(kProfileName);
  if (backend || errno != EIO) {
    error = EIO;
  }
  if (backend) {
    const int drainError = DrainControl(backend);
    if (drainError != 0) {
      error = drainError;
    }
  }
  result = kCorruptionRejected;
  Report(kCorruptor, result, error);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
