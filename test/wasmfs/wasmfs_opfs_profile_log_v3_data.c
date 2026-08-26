// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Browser-coordinated proof for the deliberately small V3 fixed-payload
// primitive. It never edits OPFS from host JavaScript. A controlled iframe
// disposal interrupts after the first or second CLEAN witness, and a fresh
// document then attaches the one exposed DataFile and observes the documented
// old/new recovery state. This is not a browser-crash or power-loss test, and
// it does not claim namespace, database, directory, or Chromium-profile
// recovery.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_MUTATOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_RESIZER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_CORRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_COMMIT_ERROR)
#error "select one V3 profile-log data test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_MUTATOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_RESIZER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_CORRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_COMMIT_ERROR) != 1
#error "select exactly one V3 profile-log data test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V3_TEST_PROFILE_NAME
#error "select a V3 profile-log data test profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V3_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V3_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V3_STRINGIFY_IMPL(value)

enum TestRole {
  kOwner,
  kMutator,
  kResizer,
  kVerifier,
  kCorruptor,
  kCommitError,
};

enum TestResult {
  kReady,
  kCorruptionRejected,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V3_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V3_TEST_PROFILE_NAME);
static const char kPayloadPath[] = "/v3-fixed-payload";
static const char kSecondPayloadPath[] = "/v3-second-payload";
static const char kDirectoryPath[] = "/v3-not-a-directory";
static const uint8_t kOldPayload[] = {'o', 'l', 'd'};
static const uint8_t kNewPayload[] = {'n', 'e', 'w'};
enum {
  kNewPayloadOffset = 16,
  kGrownPayloadSize = sizeof(kOldPayload) + 8,
};
static const struct timespec kInitialTimes[2] = {
  {42, 111000000},
  {43, 222000000},
};

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v3-data',
      },
      window.location.origin);
  }, role, result, error);
}

static void Report(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, result, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int phase) {
  EM_ASM({
    window.parent.postMessage(
      {
        event: 'interrupt',
        phase: $0,
        type: 'wasmfs-opfs-profile-log-v3-data',
      },
      window.location.origin);
  }, phase);
}
#endif

// The V3 library references this private symbol only in the matching link
// variation. Keeping a no-op in ordinary role binaries avoids a production
// hook or any host-side mutation mechanism.
void wasmfs_opfs_profile_log_v3_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT_PHASE
  if (checkpoint != WASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT_PHASE) {
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

static int OpenPayload(backend_t* backend, int* fd) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v3_data_backend(kProfileName,
                                                             0600);
  if (!*backend) {
    return ErrorOrEIO();
  }
  *fd = wasmfs_create_file(kPayloadPath, 0600, *backend);
  return *fd >= 0 ? 0 : -*fd;
}

static int DrainPayload(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static int CheckInitialMetadata(int fd) {
  struct stat status;
  if (fstat(fd, &status) != 0) {
    return ErrorOrEIO();
  }
  if (!S_ISREG(status.st_mode) || (status.st_mode & S_IALLUGO) != 0640 ||
      status.st_atim.tv_sec != kInitialTimes[0].tv_sec ||
      status.st_atim.tv_nsec != kInitialTimes[0].tv_nsec ||
      status.st_mtim.tv_sec != kInitialTimes[1].tv_sec ||
      status.st_mtim.tv_nsec != kInitialTimes[1].tv_nsec ||
      status.st_ctim.tv_sec <= 0) {
    return EIO;
  }
  return 0;
}

static int CheckPayload(int fd, int expectNew, int expectInitialMTime) {
  struct stat status;
  if (fstat(fd, &status) != 0) {
    return ErrorOrEIO();
  }
  const off_t expectedSize = expectNew
                               ? kNewPayloadOffset + sizeof(kNewPayload)
                               : sizeof(kOldPayload);
  if (!S_ISREG(status.st_mode) || (status.st_mode & S_IALLUGO) != 0640 ||
      status.st_size != expectedSize ||
      status.st_atim.tv_sec != kInitialTimes[0].tv_sec ||
      status.st_atim.tv_nsec != kInitialTimes[0].tv_nsec ||
      status.st_ctim.tv_sec <= 0 ||
      (expectInitialMTime &&
       (status.st_mtim.tv_sec != kInitialTimes[1].tv_sec ||
        status.st_mtim.tv_nsec != kInitialTimes[1].tv_nsec)) ||
      (!expectInitialMTime &&
       status.st_mtim.tv_sec < kInitialTimes[1].tv_sec)) {
    return EIO;
  }

  uint8_t contents[sizeof(kNewPayload) + kNewPayloadOffset] = {};
  const ssize_t bytes = pread(fd, contents, expectedSize, 0);
  if (bytes != expectedSize ||
      memcmp(contents, kOldPayload, sizeof(kOldPayload)) != 0) {
    return EIO;
  }
  if (expectNew) {
    for (off_t i = sizeof(kOldPayload); i != kNewPayloadOffset; ++i) {
      if (contents[i] != 0) {
        return EIO;
      }
    }
    if (memcmp(contents + kNewPayloadOffset,
               kNewPayload,
               sizeof(kNewPayload)) != 0) {
      return EIO;
    }
  }
  return 0;
}

static int CheckGrownPayload(int fd) {
  struct stat status;
  if (fstat(fd, &status) != 0) {
    return ErrorOrEIO();
  }
  if (!S_ISREG(status.st_mode) || (status.st_mode & S_IALLUGO) != 0640 ||
      status.st_size != kGrownPayloadSize ||
      status.st_atim.tv_sec != kInitialTimes[0].tv_sec ||
      status.st_atim.tv_nsec != kInitialTimes[0].tv_nsec ||
      status.st_mtim.tv_sec < kInitialTimes[1].tv_sec ||
      status.st_ctim.tv_sec <= 0) {
    return EIO;
  }
  uint8_t contents[kGrownPayloadSize] = {};
  const ssize_t bytes = pread(fd, contents, sizeof(contents), 0);
  if (bytes != sizeof(contents) ||
      memcmp(contents, kOldPayload, sizeof(kOldPayload)) != 0) {
    return EIO;
  }
  for (size_t i = sizeof(kOldPayload); i != sizeof(contents); ++i) {
    if (contents[i] != 0) {
      return EIO;
    }
  }
  return 0;
}

static int CheckBoundary(backend_t backend) {
  // V3 is intentionally not a namespace provider and may only attach one
  // DataFile into the caller's existing WasmFS namespace.
  if (wasmfs_create_directory(kDirectoryPath, 0700, backend) != -EIO ||
      wasmfs_create_file(kSecondPayloadPath, 0600, backend) != -EIO) {
    return EIO;
  }
  return 0;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int fd = -1;
  int error = 0;
  int result = kReady;

#if defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER)
  error = OpenPayload(&backend, &fd);
  if (error == 0) {
    error = CheckBoundary(backend);
  }
  if (error == 0 && write(fd, kOldPayload, sizeof(kOldPayload)) !=
                      sizeof(kOldPayload)) {
    error = ErrorOrEIO();
  }
  if (error == 0 && fchmod(fd, 0640) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0 && futimens(fd, kInitialTimes) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0) {
    error = CheckInitialMetadata(fd);
  }
  if (fd >= 0 && close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  if (backend) {
    const int drainError = DrainPayload(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kOwner, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_MUTATOR)
  error = OpenPayload(&backend, &fd);
  if (error == 0) {
    error = CheckPayload(fd, 0, 1);
  }
  if (error == 0 && pwrite(fd,
                            kNewPayload,
                            sizeof(kNewPayload),
                            kNewPayloadOffset) != sizeof(kNewPayload)) {
    error = ErrorOrEIO();
  }
  if (fd >= 0 && close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  if (backend) {
    const int drainError = DrainPayload(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kMutator, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_RESIZER)
  error = OpenPayload(&backend, &fd);
  if (error == 0) {
    error = CheckPayload(fd, 0, 1);
  }
  if (error == 0 && ftruncate(fd, kGrownPayloadSize) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0) {
    error = CheckGrownPayload(fd);
  }
  if (error == 0 && ftruncate(fd, sizeof(kOldPayload)) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0) {
    // The shrink is a second paired resize transaction. It must restore the
    // exact short byte image, while mtime/ctime legitimately advance.
    error = CheckPayload(fd, 0, 0);
  }
  if (fd >= 0 && close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  if (backend) {
    const int drainError = DrainPayload(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kResizer, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER)
  error = OpenPayload(&backend, &fd);
  const int expectNew =
#ifdef WASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_NEW
    1;
#else
    0;
#endif
  const int expectInitialMTime =
#if defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_NEW) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_RESIZED_SHORT)
    0;
#else
    1;
#endif
  if (error == 0) {
    error = CheckPayload(fd, expectNew, expectInitialMTime);
  }
#ifdef WASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_COMMIT_REJECTION
  if (error == 0) {
    errno = 0;
    if (pwrite(fd, kNewPayload, sizeof(kNewPayload), 0) != -1 ||
        errno != ESHUTDOWN) {
      error = EIO;
    }
  }
  if (error == 0) {
    error = CheckPayload(fd, 0, 1);
  }
#endif
  if (fd >= 0 && close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  if (backend) {
    const int drainError = DrainPayload(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kVerifier, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V3_TEST_COMMIT_ERROR)
  // The matching system-library variant injects a synthetic pre-write EIO.
  // It is an in-process terminal-latch test, not a physical OPFS corruption
  // or browser-crash simulation.
  int forcedFailureObserved = 0;
  error = OpenPayload(&backend, &fd);
  if (error == 0) {
    error = CheckPayload(fd, 0, 1);
  }
  if (error == 0) {
    errno = 0;
    if (pwrite(fd, kNewPayload, sizeof(kNewPayload), 0) != -1 ||
        errno != EIO) {
      error = EIO;
    } else {
      forcedFailureObserved = 1;
    }
  }
  if (error == 0) {
    errno = 0;
    const int reopened = open(kPayloadPath, O_RDONLY);
    if (reopened != -1 || errno != EIO) {
      if (reopened >= 0) {
        (void)close(reopened);
      }
      error = EIO;
    }
  }
  if (fd >= 0 && close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  if (backend && !forcedFailureObserved) {
    // If setup failed before the synthetic latch, retain the normal cleanup
    // discipline rather than masking that failure behind iframe disposal.
    const int drainError = DrainPayload(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  // A correctly observed forced latch intentionally has no successful scoped
  // handoff. The parent removes this iframe, which is the controlled document
  // teardown boundary for this synthetic in-process fault test.
  Report(kCommitError, result, error);
#else
  // A selected V3 control/manifest fault must fail the factory before a
  // generic DataFile can be attached. Failed initialisation owns its own
  // lease handoff; a surprise backend is drained for test hygiene only.
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v3_data_backend(kProfileName,
                                                            0600);
  if (backend || errno != EIO) {
    error = EIO;
  }
  if (backend) {
    const int drainError = DrainPayload(backend);
    if (drainError != 0) {
      error = drainError;
    }
  }
  result = kCorruptionRejected;
  Report(kCorruptor, result, error);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
