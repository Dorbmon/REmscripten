// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Browser-coordinated proof for the Schema-2 V4 filesystem delta format.  The
// interval-five seed creates a broad empty-file namespace.  The browser then
// runs this test's bounded target-mode advance role until read-only inspection
// finds a selected self-contained Schema-1 checkpoint; one more fresh advance
// must select a compact Schema-2 delta.  Fresh native documents prove replay
// before and after another durable mutation.  This is not physical power-loss,
// Chromium-profile, or generic profile-service evidence.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_ADVANCE) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_REPLAY) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PARENT_CORRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_ADVANCE) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_REPLAY) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_CORRUPTOR)
#error "select one V4 filesystem delta-recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_ADVANCE) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_REPLAY) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PARENT_CORRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_ADVANCE) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_REPLAY) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_CORRUPTOR) != 1
#error "select exactly one V4 filesystem delta-recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PROFILE_NAME
#error "select a V4 filesystem delta-recovery test profile name"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE)
#error "select a V4 filesystem delta-recovery interruption phase"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE) && \
  (WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE < 1 || \
   WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE > 2)
#error "invalid V4 filesystem delta-recovery interruption phase"
#endif

#if (defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_REPLAY) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RELOAD) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY_RELOAD) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_REPLAY) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_RELOAD)) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE)
#error "select the expected V4 filesystem delta-recovery target mode"
#endif

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE
#if WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE != 0600 && \
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE != 0640
#error "invalid V4 filesystem delta-recovery target mode"
#endif
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_STRINGIFY_IMPL(value) \
  #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_STRINGIFY_IMPL(value)

enum TestRole {
  kSeed,
  kAdvance,
  kReplay,
  kReload,
  kInterruptor,
  kRecovery,
  kRecoveryReload,
  kParentCorruptor,
  kExtentSeed,
  kExtentAdvance,
  kExtentReplay,
  kExtentReload,
  kExtentCorruptor,
};

enum {
  // Index 37 has inode 39.  The browser derives the selected full checkpoint
  // from the V4 artifacts rather than assuming an implementation-specific
  // mount/setup generation count.
  kFileCount = 128,
  kTargetIndex = 37,
  kTargetInode = 39,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-delta";
static const char kMarkerPath[] = "/v4fs-delta/replayed";
static const char kRecoveryPath[] = "/v4fs-delta/recovered";
static const char kExtentMarkerPath[] = "/v4fs-delta/extent-replayed";
static const uint8_t kExtentPayload[] = {
  0x75, 0x18, 0xb4, 0x3e, 0x92, 0x4f, 0xd1, 0x0b,
  0xe6, 0x39, 0x5a, 0xc7, 0x21, 0x8d, 0xf0, 0x64,
  0x0e, 0xa3, 0x57, 0xcc, 0x19, 0x76, 0xe1, 0x4a,
  0xbd, 0x02, 0x98, 0x35, 0x6f, 0xd8, 0x41, 0xae,
};
static int interruptionArmed;

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-delta-recovery',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int checkpoint) {
  EM_ASM({
    window.parent.postMessage(
      {
        checkpoint: $0,
        event: 'interrupt',
        type: 'wasmfs-opfs-profile-log-v4-filesystem-delta-recovery',
      },
      window.location.origin);
  }, checkpoint);
}
#endif

// This private test link symbol is inert until the interruptor arms it just
// before the observer-derived next metadata transaction. It is only called by
// outer V4 builds compiled with WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE
  if (!interruptionArmed || checkpoint !=
        WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPT_PHASE) {
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

static int CloseChecked(int fd) {
  return close(fd) == 0 ? 0 : ErrorOrEIO();
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

// The historical-parent and historical-extent selectors corrupt only
// stack-local native read buffers. Both roles must reject their selected delta
// during a fresh factory call with EIO, without mounting or mutating profile
// artifacts.
static int RejectCorruptedHistoricalRecord(void) {
  errno = 0;
  backend_t backend =
    wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!backend) {
    return errno == EIO ? 0 : ErrorOrEIO();
  }
  // Keep an unexpected successful factory from retaining its profile lease;
  // it is still a test failure because the selected delta was not rejected.
  const int drainError = DrainFilesystem(backend);
  return drainError ? drainError : EIO;
}

static int MakeEntryPath(int index, char* path, size_t size) {
  const int written = !path || index < 0 || index >= kFileCount
                        ? -1
                        : snprintf(path, size, "%s/entry-%03d", kMountPath,
                                   index);
  if (written < 0 || (size_t)written >= size) {
    return EIO;
  }
  return 0;
}

static int CreateEmptyFile(const char* path) {
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  return CloseChecked(fd);
}

static int CreateExtentFile(const char* path) {
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = pwrite(fd, kExtentPayload, sizeof(kExtentPayload), 0) ==
                        (ssize_t)sizeof(kExtentPayload)
                ? 0
                : ErrorOrEIO();
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int SetTargetMode(mode_t mode) {
  char target[48] = {};
  int error = MakeEntryPath(kTargetIndex, target, sizeof(target));
  const int fd = error ? -1 : open(target, O_RDONLY);
  if (!error && fd < 0) {
    error = ErrorOrEIO();
  }
  if (!error && fchmod(fd, mode) != 0) {
    error = ErrorOrEIO();
  }
  const int closeError = fd >= 0 ? CloseChecked(fd) : 0;
  return error ? error : closeError;
}

static int GetTargetModeForSize(int* mode, off_t expectedSize) {
  if (!mode || expectedSize < 0) {
    return EINVAL;
  }
  char target[48] = {};
  int error = MakeEntryPath(kTargetIndex, target, sizeof(target));
  struct stat status = {};
  if (!error && stat(target, &status) != 0) {
    error = ErrorOrEIO();
  }
  const int targetMode = status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
  if (!error && (!S_ISREG(status.st_mode) || status.st_size != expectedSize ||
                 (targetMode != 0600 && targetMode != 0640))) {
    error = EIO;
  }
  if (!error) {
    *mode = targetMode;
  }
  return error;
}

static int GetTargetMode(int* mode) {
  return GetTargetModeForSize(mode, 0);
}

static int GetExtentTargetMode(int* mode) {
  return GetTargetModeForSize(mode, sizeof(kExtentPayload));
}

static int CheckEmptyFile(const char* path, int expectedMode) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  struct stat status = {};
  int error = fstat(fd, &status) == 0 ? 0 : ErrorOrEIO();
  if (!error && (!S_ISREG(status.st_mode) || status.st_size != 0 ||
                 (expectedMode >= 0 &&
                  (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
                    expectedMode))) {
    error = EIO;
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int CheckExtentFile(int expectedMode) {
  char target[48] = {};
  int error = MakeEntryPath(kTargetIndex, target, sizeof(target));
  const int fd = error ? -1 : open(target, O_RDONLY);
  if (!error && fd < 0) {
    error = ErrorOrEIO();
  }
  struct stat status = {};
  if (!error && fstat(fd, &status) != 0) {
    error = ErrorOrEIO();
  }
  if (!error && (!S_ISREG(status.st_mode) ||
                 status.st_size != (off_t)sizeof(kExtentPayload) ||
                 (expectedMode >= 0 &&
                  (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
                    expectedMode))) {
    error = EIO;
  }
  uint8_t payload[sizeof(kExtentPayload)] = {};
  if (!error && pread(fd, payload, sizeof(payload), 0) !=
                  (ssize_t)sizeof(payload)) {
    error = ErrorOrEIO();
  }
  if (!error && memcmp(payload, kExtentPayload, sizeof(payload)) != 0) {
    error = EIO;
  }
  const int closeError = fd >= 0 ? CloseChecked(fd) : 0;
  return error ? error : closeError;
}

static int CheckBroadNamespace(int targetMode) {
  for (int index = 0; index != kFileCount; ++index) {
    char path[48] = {};
    int error = MakeEntryPath(index, path, sizeof(path));
    if (!error) {
      error = CheckEmptyFile(path, index == kTargetIndex ? targetMode : -1);
    }
    if (error) {
      return error;
    }
  }
  return 0;
}

static int CheckExtentNamespace(int targetMode) {
  for (int index = 0; index != kFileCount; ++index) {
    char path[48] = {};
    int error = MakeEntryPath(index, path, sizeof(path));
    if (!error) {
      error = index == kTargetIndex ? CheckExtentFile(targetMode)
                                    : CheckEmptyFile(path, -1);
    }
    if (error) {
      return error;
    }
  }
  return 0;
}

static int Seed(void) {
  for (int index = 0; index != kFileCount; ++index) {
    char path[48] = {};
    int error = MakeEntryPath(index, path, sizeof(path));
    if (!error) {
      error = CreateEmptyFile(path);
    }
    if (error) {
      return error;
    }
  }
  return 0;
}

static int SeedExtent(void) {
  for (int index = 0; index != kFileCount; ++index) {
    char path[48] = {};
    int error = MakeEntryPath(index, path, sizeof(path));
    if (!error) {
      error = index == kTargetIndex ? CreateExtentFile(path)
                                    : CreateEmptyFile(path);
    }
    if (error) {
      return error;
    }
  }
  return 0;
}

static int Advance(void) {
  int targetMode = 0;
  int error = CheckBroadNamespace(-1);
  if (!error) {
    error = GetTargetMode(&targetMode);
  }
  if (!error) {
    targetMode = targetMode == 0600 ? 0640 : 0600;
    error = SetTargetMode(targetMode);
  }
  if (!error) {
    error = CheckBroadNamespace(targetMode);
  }
  return error;
}

static int AdvanceExtent(void) {
  int targetMode = 0;
  int error = CheckExtentNamespace(-1);
  if (!error) {
    error = GetExtentTargetMode(&targetMode);
  }
  if (!error) {
    targetMode = targetMode == 0600 ? 0640 : 0600;
    error = SetTargetMode(targetMode);
  }
  if (!error) {
    error = CheckExtentNamespace(targetMode);
  }
  return error;
}

static int Replay(int targetMode) {
  int error = CheckBroadNamespace(targetMode);
  if (!error) {
    error = CreateEmptyFile(kMarkerPath);
  }
  if (!error) {
    error = CheckEmptyFile(kMarkerPath, 0600);
  }
  return error;
}

static int Reload(int targetMode) {
  int error = CheckBroadNamespace(targetMode);
  if (!error) {
    error = CheckEmptyFile(kMarkerPath, 0600);
  }
  return error;
}

static int ReplayExtent(int targetMode) {
  int error = CheckExtentNamespace(targetMode);
  if (!error) {
    error = CreateEmptyFile(kExtentMarkerPath);
  }
  if (!error) {
    error = CheckEmptyFile(kExtentMarkerPath, 0600);
  }
  return error;
}

static int ReloadExtent(int targetMode) {
  int error = CheckExtentNamespace(targetMode);
  if (!error) {
    error = CheckEmptyFile(kExtentMarkerPath, 0600);
  }
  return error;
}

static int Interrupt(void) {
  int targetMode = 0;
  int error = CheckBroadNamespace(-1);
  if (!error) {
    error = GetTargetMode(&targetMode);
  }
  if (!error) {
    interruptionArmed = 1;
  }
  // The browser has just observed a self-contained checkpoint. The hook never
  // returns once the requested next-generation outer V4 witness is written.
  if (!error) {
    error = SetTargetMode(targetMode == 0600 ? 0640 : 0600);
  }
  return error ? error : EIO;
}

static int Recover(int targetMode) {
  int error = CheckBroadNamespace(targetMode);
  if (!error) {
    error = CreateEmptyFile(kRecoveryPath);
  }
  if (!error) {
    error = CheckEmptyFile(kRecoveryPath, 0600);
  }
  return error;
}

static int ReloadRecovery(int targetMode) {
  int error = CheckBroadNamespace(targetMode);
  if (!error) {
    error = CheckEmptyFile(kRecoveryPath, 0600);
  }
  return error;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;
  int role = kSeed;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PARENT_CORRUPTOR) || \
  defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_CORRUPTOR)
  error = RejectCorruptedHistoricalRecord();
#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PARENT_CORRUPTOR)
  role = kParentCorruptor;
#else
  role = kExtentCorruptor;
#endif
#else
  error = MountFilesystem(&backend);
#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_SEED)
  if (!error) {
    error = Seed();
  }
  role = kSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_ADVANCE)
  if (!error) {
    error = Advance();
  }
  role = kAdvance;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_REPLAY)
  if (!error) {
    error = Replay(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE);
  }
  role = kReplay;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RELOAD)
  if (!error) {
    error = Reload(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE);
  }
  role = kReload;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPTOR)
  if (!error) {
    error = Interrupt();
  }
  role = kInterruptor;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY)
  if (!error) {
    error = Recover(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE);
  }
  role = kRecovery;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY_RELOAD)
  if (!error) {
    error = ReloadRecovery(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE);
  }
  role = kRecoveryReload;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_SEED)
  if (!error) {
    error = SeedExtent();
  }
  role = kExtentSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_ADVANCE)
  if (!error) {
    error = AdvanceExtent();
  }
  role = kExtentAdvance;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_REPLAY)
  if (!error) {
    error = ReplayExtent(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE);
  }
  role = kExtentReplay;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_RELOAD)
  if (!error) {
    error = ReloadExtent(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXPECT_TARGET_MODE);
  }
  role = kExtentReload;
#else
#error "unhandled V4 filesystem delta-recovery test role"
#endif
#endif

  if (backend) {
    const int drainError = DrainFilesystem(backend);
    if (!error) {
      error = drainError;
    }
  }
  Report(role, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
