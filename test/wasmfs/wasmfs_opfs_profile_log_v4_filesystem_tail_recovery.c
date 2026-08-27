// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document unreachable-tail recovery proof for a mounted V4 logical
// filesystem. A seed document commits a small file. An interruptor stops at
// the first V4 phase witness of a two-chunk overwrite, leaving a complete old
// selected state and unreachable records beyond its high-water mark. A fresh
// quota-injected document must reject the tail trim without reporting a
// successful mutation. A later normal document validates the old state,
// reclaims the tail before committing a new file, and a final document reopens
// that result. The parent independently reads only the named test artifacts to
// verify the physical bounds. This is controlled iframe-disposal evidence, not
// physical quota, power-loss, database, or Chromium-profile recovery evidence.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_TRIM_FAILURE) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_RELOAD)
#error "select one V4 filesystem tail-recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_TRIM_FAILURE) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_RELOAD) != 1
#error "select exactly one V4 filesystem tail-recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_PROFILE_NAME
#error "select a V4 filesystem tail-recovery test profile name"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPT_PHASE)
#error "select a V4 filesystem tail-recovery interruption phase"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPT_PHASE) && \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPT_PHASE != 1
#error "the V4 filesystem tail-recovery test stops only at phase one"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_STRINGIFY_IMPL( \
  value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_STRINGIFY_IMPL(value)

enum TestRole {
  kSeed,
  kInterruptor,
  kTrimFailure,
  kVerifier,
  kReload,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-tail-recovery";
static const char kDataPath[] = "/v4fs-tail-recovery/data";
static const char kIdentityPath[] = "/v4fs-tail-recovery/identity";
static const char kPostRecoveryPath[] = "/v4fs-tail-recovery/post-recovery";
static const char kTrimFailurePath[] = "/v4fs-tail-recovery/trim-failure";
static const uint8_t kOldData[] = "V4 selected bytes before unreachable tail";
static const uint8_t kPostRecoveryData[] =
  "V4 committed bytes after unreachable-tail trim";
static uint8_t kInterruptedData[2 * 64 * 1024];

struct Identity {
  uint8_t magic[8];
  uint32_t schema;
  uint32_t reserved;
  uint64_t data_ino;
};

static int test_stage;

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        stage: $2,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-tail-recovery',
      },
      window.location.origin);
  }, role, error, test_stage);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int checkpoint) {
  EM_ASM({
    window.parent.postMessage(
      {
        checkpoint: $0,
        event: 'interrupt',
        type: 'wasmfs-opfs-profile-log-v4-filesystem-tail-recovery',
      },
      window.location.origin);
  }, checkpoint);
}
#endif

// This private link symbol exists only in the interruption test build. The
// production library has no interruption route or host-side mutation API.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPT_PHASE
  if (checkpoint !=
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPT_PHASE) {
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
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int CreateFile(const char* path,
                      const void* bytes,
                      size_t size,
                      uint64_t* ino) {
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, bytes, size, 0);
  struct stat status = {};
  if (!error && fstat(fd, &status) != 0) {
    error = ErrorOrEIO();
  }
  if (!error && (!S_ISREG(status.st_mode) || !status.st_ino ||
                 status.st_size != (off_t)size)) {
    error = EIO;
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }
  if (ino) {
    *ino = status.st_ino;
  }
  return 0;
}

static int ReadIdentity(struct Identity* identity) {
  if (!identity) {
    return EINVAL;
  }
  const int fd = open(kIdentityPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = ReadExact(fd, identity, sizeof(*identity), 0);
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }
  return memcmp(identity->magic, "WFSV4TAL", sizeof(identity->magic)) == 0 &&
         identity->schema == 1 && identity->reserved == 0 && identity->data_ino
           ? 0
           : EIO;
}

static int CheckFile(const char* path,
                     const void* expected,
                     size_t size,
                     uint64_t expected_ino) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  struct stat status = {};
  int error = fstat(fd, &status) == 0 ? 0 : ErrorOrEIO();
  if (!error && (!S_ISREG(status.st_mode) || status.st_size != (off_t)size ||
                 (expected_ino && (uint64_t)status.st_ino != expected_ino))) {
    error = EIO;
  }
  uint8_t actual[96] = {};
  if (!error && size > sizeof(actual)) {
    error = EIO;
  }
  if (!error) {
    error = ReadExact(fd, actual, size, 0);
  }
  if (!error && memcmp(actual, expected, size) != 0) {
    error = EIO;
  }
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int CheckCommittedState(void) {
  struct Identity identity = {};
  int error = ReadIdentity(&identity);
  if (!error) {
    error = CheckFile(kDataPath, kOldData, sizeof(kOldData) - 1,
                      identity.data_ino);
  }
  return error;
}

static int SeedFilesystem(void) {
  struct Identity identity = {};
  memcpy(identity.magic, "WFSV4TAL", sizeof(identity.magic));
  identity.schema = 1;
  int error = CreateFile(kDataPath, kOldData, sizeof(kOldData) - 1,
                         &identity.data_ino);
  if (!error) {
    error = CreateFile(kIdentityPath, &identity, sizeof(identity), NULL);
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int InterruptOverwrite(void) {
  for (size_t index = 0; index != sizeof(kInterruptedData); ++index) {
    kInterruptedData[index] = (uint8_t)('a' + index % 26);
  }
  const int fd = open(kDataPath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  // The V4 interruption hook stops inside this pwrite after phase witness
  // one. A normal return would mean it did not reach that native boundary.
  const int error = WriteExact(fd, kInterruptedData, sizeof(kInterruptedData),
                               0);
  const int close_error = CloseChecked(fd);
  return error ? error : close_error ? close_error : EIO;
}

static int ObserveTailTrimFailure(void) {
  test_stage = 1;
  int error = CheckCommittedState();
  if (!error) {
    // This namespace mutation's first V4 transaction must trim the known
    // unselected tail. The test-only bridge fails that pre-native truncate;
    // it must report the bridge's ENOSPC translation, then terminal syscall
    // admission must reject a later read-only operation.
    test_stage = 2;
    errno = 0;
    if (mkdir(kTrimFailurePath, 0700) != -1 || errno != ENOSPC) {
      error = EIO;
    }
    if (!error) {
      errno = 0;
      const int fd = open(kDataPath, O_RDONLY);
      if (fd >= 0) {
        (void)CloseChecked(fd);
        error = EIO;
      }
    }
  }
  return error;
}

static int CommitPostRecoveryMutation(void) {
  int error = CreateFile(kPostRecoveryPath, kPostRecoveryData,
                         sizeof(kPostRecoveryData) - 1, NULL);
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend);
  int role = kSeed;
  int observed_expected_trim_failure = 0;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_SEED)
  if (!error) {
    error = SeedFilesystem();
  }
  role = kSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPTOR)
  if (!error) {
    error = CheckCommittedState();
  }
  if (!error) {
    error = InterruptOverwrite();
  }
  role = kInterruptor;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_TRIM_FAILURE)
  if (!error) {
    error = ObserveTailTrimFailure();
  }
  observed_expected_trim_failure = error == 0;
  role = kTrimFailure;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_VERIFIER)
  if (!error) {
    error = CheckCommittedState();
  }
  if (!error) {
    error = CommitPostRecoveryMutation();
  }
  role = kVerifier;
#else
  if (!error) {
    error = CheckCommittedState();
  }
  if (!error) {
    error = CheckFile(kPostRecoveryPath, kPostRecoveryData,
                      sizeof(kPostRecoveryData) - 1, 0);
  }
  if (!error) {
    error = FlushDirectory();
  }
  role = kReload;
#endif

  // The injected trim failure poisons the backend by design, so it cannot
  // perform a clean drain. The parent discards only that iframe; every normal
  // role proves an ordinary result-bearing drain.
  if (backend && !observed_expected_trim_failure) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(role, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
