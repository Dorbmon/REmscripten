// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document error-propagation proof for a mounted V4 logical-profile
// filesystem. A normal document seeds and drains a profile. A second document
// uses the test-only OPFS quota hook for its first data mutation and must see
// ENOSPC, after which the attached filesystem rejects further operations. The
// parent disposes that poisoned document, then fresh normal documents prove
// the exact committed old state and a subsequent durable mutation. The hook
// models browser QuotaExceededError translation; it is not physical quota,
// power-loss, database, or Chromium-profile evidence.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_QUOTA) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_RELOAD)
#error "select one V4 filesystem quota-recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_QUOTA) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_RELOAD) != 1
#error "select exactly one V4 filesystem quota-recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_PROFILE_NAME
#error "select a V4 filesystem quota-recovery test profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_STRINGIFY_IMPL( \
  value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_STRINGIFY_IMPL(value)

enum TestRole {
  kSeed,
  kQuota,
  kVerifier,
  kReload,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-quota-recovery";
static const char kDataPath[] = "/v4fs-quota-recovery/data";
static const char kIdentityPath[] = "/v4fs-quota-recovery/identity";
static const char kPostErrorPath[] = "/v4fs-quota-recovery/post-error";
static const uint8_t kOldData[] = "V4 committed bytes before quota failure";
static const uint8_t kNewData[] = "V4 rejected bytes after quota failure";
static const uint8_t kPostErrorData[] = "V4 fresh document after quota failure";

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
        type: 'wasmfs-opfs-profile-log-v4-filesystem-quota-recovery',
      },
      window.location.origin);
  }, role, error, test_stage);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
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
  return memcmp(identity->magic, "WFSV4QTA", sizeof(identity->magic)) == 0 &&
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
  memcpy(identity.magic, "WFSV4QTA", sizeof(identity.magic));
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

static int IsTerminalFailure(int result) {
  // The failing pwrite reports its physical quota cause. Subsequent syscall
  // admission sees the poisoned logical backend, whose generic terminal
  // error is EIO today; retaining ENOSPC is also an acceptable future
  // refinement. Neither result may be reported as a successful flush/open.
  return result == -1 && (errno == EIO || errno == ENOSPC);
}

static int ObserveQuotaFailure(void) {
  test_stage = 1;
  int error = CheckCommittedState();
  int fd = -1;
  if (!error) {
    test_stage = 2;
    fd = open(kDataPath, O_RDWR);
    if (fd < 0) {
      error = ErrorOrEIO();
    }
  }
  if (!error) {
    test_stage = 3;
    errno = 0;
    if (pwrite(fd, kNewData, sizeof(kNewData) - 1, 0) != -1 ||
        errno != ENOSPC) {
      error = EIO;
    }
  }
  // The failed transaction must latch the mounted backend: a later flush and
  // open cannot report success while its logical state is no longer writable.
  if (!error) {
    test_stage = 4;
    errno = 0;
    if (!IsTerminalFailure(fdatasync(fd))) {
      error = EIO;
    }
  }
  if (fd >= 0) {
    (void)close(fd);
  }
  if (!error) {
    test_stage = 5;
    errno = 0;
    const int reopened = open(kDataPath, O_RDONLY);
    if (!IsTerminalFailure(reopened)) {
      if (reopened >= 0) {
        (void)close(reopened);
      }
      error = EIO;
    }
  }
  return error;
}

static int CommitPostErrorMutation(void) {
  int error = CreateFile(kPostErrorPath, kPostErrorData,
                         sizeof(kPostErrorData) - 1, NULL);
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
  int observed_expected_failure = 0;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_SEED)
  if (!error) {
    error = SeedFilesystem();
  }
  role = kSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_QUOTA)
  if (!error) {
    error = ObserveQuotaFailure();
  }
  observed_expected_failure = error == 0;
  role = kQuota;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_VERIFIER)
  if (!error) {
    error = CheckCommittedState();
  }
  if (!error) {
    error = CommitPostErrorMutation();
  }
  role = kVerifier;
#else
  if (!error) {
    error = CheckCommittedState();
  }
  if (!error) {
    error = CheckFile(kPostErrorPath, kPostErrorData,
                      sizeof(kPostErrorData) - 1, 0);
  }
  if (!error) {
    error = FlushDirectory();
  }
  role = kReload;
#endif

  // A correctly observed quota latch intentionally cannot make a clean
  // backend handoff. The parent disposes only that iframe; a fresh document
  // must then recover the last selected generation. If setup did not reach
  // the expected latch, retain normal test cleanup instead.
  if (backend && !observed_expected_failure) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(role, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
