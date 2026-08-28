// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document checkpoint/reclamation proof for the mountable V4 logical
// filesystem. Test builds use an odd three-generation checkpoint cadence so
// two small, deterministic cycles exercise both physical arenas. The parent
// independently observes only fixed OPFS artifacts after orderly drains; all
// logical state and recovery checks run through fresh native mounts. Separate
// roles interrupt a scheduled checkpoint after each V4 phase witness, and an
// open-unlink role verifies that a prospective orphan vetoes reclamation until
// the descriptor has closed. This is controlled iframe-disposal evidence, not
// physical power-loss or Chromium-profile recovery evidence.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_ONE) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_TWO) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PRODUCTION) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PRODUCTION_RELOAD) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT_RELOAD)
#error "select one V4 filesystem checkpoint-recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_ONE) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_TWO) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PRODUCTION) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PRODUCTION_RELOAD) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT_RELOAD) != 1
#error "select exactly one V4 filesystem checkpoint-recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PROFILE_NAME
#error "select a V4 filesystem checkpoint-recovery test profile name"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE)
#error "select a V4 filesystem checkpoint-recovery interruption phase"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE) && \
  (WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE < 1 || \
   WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE > 2)
#error "invalid V4 filesystem checkpoint-recovery interruption phase"
#endif

#if (defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_VERIFIER) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_RELOAD)) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_EXPECT_ROUND_ONE)
#error "select the expected V4 filesystem checkpoint-recovery state"
#endif

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_EXPECT_ROUND_ONE
#if WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_EXPECT_ROUND_ONE < 0 || \
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_EXPECT_ROUND_ONE > 1
#error "invalid expected V4 filesystem checkpoint-recovery state"
#endif
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_STRINGIFY_IMPL( \
  value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_STRINGIFY_IMPL(value)

enum TestRole {
  kSeed,
  kRoundOne,
  kRoundTwo,
  kVerifier,
  kInterruptor,
  kRecoveryVerifier,
  kRecoveryReload,
  kOrphan,
  kOrphanReload,
  kProduction,
  kProductionReload,
  kReplacement,
  kReplacementReload,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-checkpoint";
static const char kAnchorPath[] = "/v4fs-checkpoint/anchor";
static const char kRoundOnePath[] = "/v4fs-checkpoint/round-one";
static const char kRoundTwoPath[] = "/v4fs-checkpoint/round-two";
static const char kRecoveryPath[] = "/v4fs-checkpoint/recovery";
static const char kOrphanOnePath[] = "/v4fs-checkpoint/orphan-one";
static const char kOrphanTwoPath[] = "/v4fs-checkpoint/orphan-two";
static const char kOrphanThreePath[] = "/v4fs-checkpoint/orphan-three";
static const char kReplacementSourcePath[] =
  "/v4fs-checkpoint/replacement-source";
static const char kReplacementOnePath[] = "/v4fs-checkpoint/replacement-one";
static const char kReplacementTwoPath[] = "/v4fs-checkpoint/replacement-two";
static const char kReplacementThreePath[] =
  "/v4fs-checkpoint/replacement-three";
static const char kReplacementFourPath[] =
  "/v4fs-checkpoint/replacement-four";
static const uint8_t kDataA[] = "checkpoint-before-copy";
static const uint8_t kDataB[] = "checkpoint-live-extent";
static const uint8_t kDataC[] = "checkpoint-next-extent";
static const uint8_t kDataD[] = "checkpoint-after-second-copy";
static const uint8_t kReplacementData[] = "checkpoint-replacement-source";
static int interruptionArmed;

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-checkpoint-recovery',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int checkpoint) {
  EM_ASM({
    window.parent.postMessage(
      {
        checkpoint: $0,
        event: 'interrupt',
        type: 'wasmfs-opfs-profile-log-v4-filesystem-checkpoint-recovery',
      },
      window.location.origin);
  }, checkpoint);
}
#endif

// This private link symbol exists only in builds that also select the outer
// V4 interruption setting. It is deliberately inert until the test arms it
// immediately before the known scheduled checkpoint transaction.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE
  if (!interruptionArmed || checkpoint !=
        WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPT_PHASE) {
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

static int WriteExact(int fd, const void* bytes, size_t size, off_t offset) {
  return pwrite(fd, bytes, size, offset) == (ssize_t)size ? 0 : ErrorOrEIO();
}

static int ReadExact(int fd, void* bytes, size_t size, off_t offset) {
  return pread(fd, bytes, size, offset) == (ssize_t)size ? 0 : ErrorOrEIO();
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

static int FlushDirectory(void) {
  const int fd = open(kMountPath, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  const int error = fsync(fd) == 0 ? 0 : ErrorOrEIO();
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int CreateEmptyFile(const char* path) {
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  return CloseChecked(fd);
}

static int CheckEmptyFile(const char* path) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  struct stat status = {};
  int error = fstat(fd, &status) == 0 ? 0 : ErrorOrEIO();
  if (!error && (!S_ISREG(status.st_mode) || status.st_size != 0)) {
    error = EIO;
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int CheckAbsent(const char* path) {
  struct stat status = {};
  errno = 0;
  return lstat(path, &status) == -1 && errno == ENOENT ? 0 : EIO;
}

static int CheckAnchor(const uint8_t* expected, size_t size) {
  const int fd = open(kAnchorPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  struct stat status = {};
  int error = fstat(fd, &status) == 0 ? 0 : ErrorOrEIO();
  if (!error && (!S_ISREG(status.st_mode) || status.st_size != (off_t)size)) {
    error = EIO;
  }
  uint8_t actual[64] = {};
  if (!error && size > sizeof(actual)) {
    error = EIO;
  }
  if (!error) {
    error = ReadExact(fd, actual, size, 0);
  }
  if (!error && memcmp(actual, expected, size) != 0) {
    error = EIO;
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int WriteAnchor(const uint8_t* bytes, size_t size) {
  const int fd = open(kAnchorPath, O_WRONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, bytes, size, 0);
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int Seed(void) {
  const int fd = open(kAnchorPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, kDataA, sizeof(kDataA) - 1, 0);
  if (!error) {
    error = WriteExact(fd, kDataB, sizeof(kDataB) - 1, 0);
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int closeError = CloseChecked(fd);
  if (error || closeError) {
    return error ? error : closeError;
  }
  return FlushDirectory();
}

static int RoundOne(void) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (!error) {
    error = CreateEmptyFile(kRoundOnePath);
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int RoundTwo(void) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (!error) {
    error = CheckEmptyFile(kRoundOnePath);
  }
  if (!error) {
    error = WriteAnchor(kDataC, sizeof(kDataC) - 1);
  }
  if (!error) {
    error = WriteAnchor(kDataD, sizeof(kDataD) - 1);
  }
  if (!error) {
    error = CreateEmptyFile(kRoundTwoPath);
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int VerifyFinalState(void) {
  int error = CheckAnchor(kDataD, sizeof(kDataD) - 1);
  if (!error) {
    error = CheckEmptyFile(kRoundOnePath);
  }
  if (!error) {
    error = CheckEmptyFile(kRoundTwoPath);
  }
  return error;
}

static int InterruptRoundOne(void) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (!error) {
    error = CheckAbsent(kRoundOnePath);
  }
  if (error) {
    return error;
  }
  interruptionArmed = 1;
  // This is g6 under the test cadence: it copies the g5 live anchor extent
  // before the V4 phase hook fires. A return means the selected witness was
  // not reached and is a test failure.
  error = CreateEmptyFile(kRoundOnePath);
  return error ? error : EIO;
}

static int VerifyRecoveryState(int expectRoundOne) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (!error) {
    error = expectRoundOne ? CheckEmptyFile(kRoundOnePath)
                            : CheckAbsent(kRoundOnePath);
  }
  if (!error) {
    error = CreateEmptyFile(kRecoveryPath);
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int ReloadRecoveryState(int expectRoundOne) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (!error) {
    error = expectRoundOne ? CheckEmptyFile(kRoundOnePath)
                            : CheckAbsent(kRoundOnePath);
  }
  if (!error) {
    error = CheckEmptyFile(kRecoveryPath);
  }
  return error;
}

static int OpenUnlinkBoundary(void) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (error) {
    return error;
  }
  const int fd = open(kAnchorPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  if (unlink(kAnchorPath) != 0) {
    error = ErrorOrEIO();
  }
  uint8_t actual[64] = {};
  if (!error) {
    error = ReadExact(fd, actual, sizeof(kDataB) - 1, 0);
  }
  if (!error && memcmp(actual, kDataB, sizeof(kDataB) - 1) != 0) {
    error = EIO;
  }
  const int closeError = CloseChecked(fd);
  if (error || closeError) {
    return error ? error : closeError;
  }
  if ((error = CheckAbsent(kAnchorPath)) ||
      (error = CreateEmptyFile(kOrphanOnePath)) ||
      (error = CreateEmptyFile(kOrphanTwoPath)) ||
      (error = CreateEmptyFile(kOrphanThreePath))) {
    return error;
  }
  return FlushDirectory();
}

static int ReloadOrphanState(void) {
  int error = CheckAbsent(kAnchorPath);
  if (!error) {
    error = CheckEmptyFile(kOrphanOnePath);
  }
  if (!error) {
    error = CheckEmptyFile(kOrphanTwoPath);
  }
  if (!error) {
    error = CheckEmptyFile(kOrphanThreePath);
  }
  return error;
}

static int CreateReplacementSource(void) {
  const int fd = open(kReplacementSourcePath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, kReplacementData, sizeof(kReplacementData) - 1,
                         0);
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int OpenReplacementBoundary(void) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (error) {
    return error;
  }
  const int fd = open(kAnchorPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  // Seed is g5. Creating and writing the source reaches g7, then the first
  // marker reaches g8. The replacement rename is the g9 checkpoint boundary,
  // but it deliberately defers reclamation while the old anchor descriptor is
  // open; the later g12 checkpoint reclaims only after that descriptor closes.
  if (!error) {
    error = CreateReplacementSource();
  }
  if (!error) {
    error = CreateEmptyFile(kReplacementOnePath);
  }
  if (!error && rename(kReplacementSourcePath, kAnchorPath) != 0) {
    error = ErrorOrEIO();
  }
  if (error) {
    const int closeError = CloseChecked(fd);
    return error ? error : closeError;
  }
  uint8_t actual[64] = {};
  error = ReadExact(fd, actual, sizeof(kDataB) - 1, 0);
  if (!error && memcmp(actual, kDataB, sizeof(kDataB) - 1) != 0) {
    error = EIO;
  }
  const int closeError = CloseChecked(fd);
  if (error || closeError) {
    return error ? error : closeError;
  }
  if ((error = CheckAnchor(kReplacementData, sizeof(kReplacementData) - 1)) ||
      (error = CheckAbsent(kReplacementSourcePath)) ||
      (error = CheckEmptyFile(kReplacementOnePath)) ||
      (error = CreateEmptyFile(kReplacementTwoPath)) ||
      (error = CreateEmptyFile(kReplacementThreePath)) ||
      (error = CreateEmptyFile(kReplacementFourPath))) {
    return error;
  }
  return FlushDirectory();
}

static int ReloadReplacementState(void) {
  int error = CheckAnchor(kReplacementData, sizeof(kReplacementData) - 1);
  if (!error) {
    error = CheckAbsent(kReplacementSourcePath);
  }
  if (!error) {
    error = CheckEmptyFile(kReplacementOnePath);
  }
  if (!error) {
    error = CheckEmptyFile(kReplacementTwoPath);
  }
  if (!error) {
    error = CheckEmptyFile(kReplacementThreePath);
  }
  if (!error) {
    error = CheckEmptyFile(kReplacementFourPath);
  }
  return error;
}

static int CheckAnchorMode(mode_t expectedMode) {
  const int fd = open(kAnchorPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  struct stat status = {};
  int error = fstat(fd, &status) == 0 ? 0 : ErrorOrEIO();
  if (!error && (!S_ISREG(status.st_mode) ||
                 (status.st_mode & S_IALLUGO) != expectedMode)) {
    error = EIO;
  }
  const int closeError = CloseChecked(fd);
  return error ? error : closeError;
}

static int ExerciseProductionCadence(void) {
  // Seed leaves the selected logical generation at g5. Each alternating mode
  // write below makes exactly one durable transaction, so the 57 calls end at
  // g62 after real g31/a1 and g62/a0 checkpoints under the production odd
  // cadence. The anchor's one immutable extent must survive both copies.
  int error = Seed();
  if (error) {
    return error;
  }
  const int fd = open(kAnchorPath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  for (int index = 0; !error && index != 57; ++index) {
    if (fchmod(fd, index & 1 ? 0600 : 0640) != 0) {
      error = ErrorOrEIO();
    }
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int closeError = CloseChecked(fd);
  if (error || closeError) {
    return error ? error : closeError;
  }
  if ((error = CheckAnchor(kDataB, sizeof(kDataB) - 1))) {
    return error;
  }
  if ((error = CheckAnchorMode(0640))) {
    return error;
  }
  return FlushDirectory();
}

static int ReloadProductionCadence(void) {
  int error = CheckAnchor(kDataB, sizeof(kDataB) - 1);
  if (!error) {
    error = CheckAnchorMode(0640);
  }
  return error;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend);
  int role = kSeed;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED)
  if (!error) {
    error = Seed();
  }
  role = kSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_ONE)
  if (!error) {
    error = RoundOne();
  }
  role = kRoundOne;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_TWO)
  if (!error) {
    error = RoundTwo();
  }
  role = kRoundTwo;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_VERIFIER)
  if (!error) {
    error = VerifyFinalState();
  }
  role = kVerifier;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPTOR)
  if (!error) {
    error = InterruptRoundOne();
  }
  role = kInterruptor;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_VERIFIER)
  if (!error) {
    error = VerifyRecoveryState(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_EXPECT_ROUND_ONE);
  }
  role = kRecoveryVerifier;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_RECOVERY_RELOAD)
  if (!error) {
    error = ReloadRecoveryState(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_EXPECT_ROUND_ONE);
  }
  role = kRecoveryReload;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN)
  if (!error) {
    error = OpenUnlinkBoundary();
  }
  role = kOrphan;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN_RELOAD)
  if (!error) {
    error = ReloadOrphanState();
  }
  role = kOrphanReload;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PRODUCTION)
  if (!error) {
    error = ExerciseProductionCadence();
  }
  role = kProduction;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT)
  if (!error) {
    error = OpenReplacementBoundary();
  }
  role = kReplacement;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT_RELOAD)
  if (!error) {
    error = ReloadReplacementState();
  }
  role = kReplacementReload;
#else
  if (!error) {
    error = ReloadProductionCadence();
  }
  role = kProductionReload;
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
