// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document recovery proof for a mounted V4 logical-profile filesystem.
// A seed document commits a small inode tree. A second document stops after
// either phase witness of a data overwrite or a replacement rename, and the
// parent disposes that document. A fresh document must then expose precisely
// the selected old or new tree, commit a further directory mutation, and a
// final fresh document must reopen that result. This is a controlled
// iframe-disposal test, not a physical-power-loss or Chrome-profile claim.

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

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_SEED) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RELOAD)
#error "select one V4 filesystem mutation-recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_SEED) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RELOAD) != 1
#error "select exactly one V4 filesystem mutation-recovery test role"
#endif

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RENAME)
#error "select one V4 filesystem mutation-recovery test kind"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RENAME) != 1
#error "select exactly one V4 filesystem mutation-recovery test kind"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_PROFILE_NAME
#error "select a V4 filesystem mutation-recovery test profile name"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPT_PHASE)
#error "select a V4 filesystem mutation-recovery interruption phase"
#endif

#if (defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_VERIFIER) || \
     defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RELOAD)) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_EXPECT_NEW)
#error "select the expected V4 filesystem mutation-recovery state"
#endif

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_EXPECT_NEW
#if WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_EXPECT_NEW < 0 || \
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_EXPECT_NEW > 1
#error "invalid expected V4 filesystem mutation-recovery state"
#endif
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_STRINGIFY_IMPL(value) \
  #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_STRINGIFY_IMPL(value)

enum TestRole {
  kSeed,
  kInterruptor,
  kVerifier,
  kReload,
};

enum MutationKind {
  kDataMutation = 1,
  kRenameMutation = 2,
};

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA)
static const uint32_t kMutationKind = kDataMutation;
#else
static const uint32_t kMutationKind = kRenameMutation;
#endif

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-mutation-recovery";
static const char kDataPath[] = "/v4fs-mutation-recovery/data";
static const char kSourcePath[] = "/v4fs-mutation-recovery/source";
static const char kDestinationPath[] = "/v4fs-mutation-recovery/destination";
static const char kIdentityPath[] = "/v4fs-mutation-recovery/identity";
static const char kPostRecoveryPath[] = "/v4fs-mutation-recovery/post-recovery";
static const uint8_t kOldData[] = "V4-data-before";
static const uint8_t kNewData[] = "V4-data-after!";
static const uint8_t kSourceData[] = "V4-rename-source";
static const uint8_t kDestinationData[] = "V4-rename-target";
static const uint8_t kPostRecoveryData[] = "V4-post-recovery";

struct Identity {
  uint8_t magic[8];
  uint32_t schema;
  uint32_t mutation;
  uint64_t primary_ino;
  uint64_t secondary_ino;
};

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-mutation-recovery',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int checkpoint) {
  EM_ASM({
    window.parent.postMessage(
      {
        checkpoint: $0,
        event: 'interrupt',
        type: 'wasmfs-opfs-profile-log-v4-filesystem-mutation-recovery',
      },
      window.location.origin);
  }, checkpoint);
}
#endif

// This private link symbol exists only in a test build selected with
// -sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1. Production builds expose no
// interruption route and never link this hook.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPT_PHASE
  if (checkpoint !=
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPT_PHASE) {
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

static int FlushDirectory(void) {
  const int fd = open(kMountPath, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = fsync(fd) == 0 ? 0 : ErrorOrEIO();
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

static int ReadFile(const char* path,
                    void* bytes,
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
  if (!error) {
    error = ReadExact(fd, bytes, size, 0);
  }
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int CheckFile(const char* path,
                     const void* expected,
                     size_t size,
                     uint64_t expected_ino) {
  uint8_t actual[64] = {};
  if (size > sizeof(actual)) {
    return EIO;
  }
  int error = ReadFile(path, actual, size, expected_ino);
  if (error) {
    return error;
  }
  return memcmp(actual, expected, size) == 0 ? 0 : EIO;
}

static int CheckAbsent(const char* path) {
  struct stat status = {};
  errno = 0;
  return lstat(path, &status) == -1 && errno == ENOENT ? 0 : EIO;
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

static int ReadIdentity(struct Identity* identity) {
  if (!identity) {
    return EINVAL;
  }
  int error = ReadFile(kIdentityPath, identity, sizeof(*identity), 0);
  if (error) {
    return error;
  }
  return memcmp(identity->magic, "WFSV4TXN", sizeof(identity->magic)) == 0 &&
         identity->schema == 1 && identity->mutation == kMutationKind &&
         identity->primary_ino &&
         (kMutationKind == kDataMutation || identity->secondary_ino)
           ? 0
           : EIO;
}

static int SeedFilesystem(void) {
  struct Identity identity = {};
  memcpy(identity.magic, "WFSV4TXN", sizeof(identity.magic));
  identity.schema = 1;
  identity.mutation = kMutationKind;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA)
  int error = CreateFile(kDataPath, kOldData, sizeof(kOldData),
                         &identity.primary_ino);
#else
  int error = CreateFile(kSourcePath, kSourceData, sizeof(kSourceData),
                         &identity.primary_ino);
  if (!error) {
    error = CreateFile(kDestinationPath, kDestinationData,
                       sizeof(kDestinationData), &identity.secondary_ino);
  }
#endif
  if (!error) {
    error = CreateFile(kIdentityPath, &identity, sizeof(identity), NULL);
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int VerifySelectedState(int expect_new) {
  struct Identity identity = {};
  int error = ReadIdentity(&identity);
  if (error) {
    return error;
  }

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA)
  const void* expected = expect_new ? kNewData : kOldData;
  const size_t expected_size = expect_new ? sizeof(kNewData) : sizeof(kOldData);
  return CheckFile(kDataPath, expected, expected_size, identity.primary_ino);
#else
  if (!expect_new) {
    error = CheckFile(kSourcePath, kSourceData, sizeof(kSourceData),
                      identity.primary_ino);
    if (error) {
      return error;
    }
    return CheckFile(kDestinationPath, kDestinationData,
                     sizeof(kDestinationData), identity.secondary_ino);
  }
  error = CheckAbsent(kSourcePath);
  if (error) {
    return error;
  }
  return CheckFile(kDestinationPath, kSourceData, sizeof(kSourceData),
                   identity.primary_ino);
#endif
}

static int RunInterruptedMutation(void) {
#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA)
  const int fd = open(kDataPath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  // The selected interruption is inside the atomic pwrite transaction. If it
  // returns, the test did not stop at the requested native witness boundary.
  const int error = WriteExact(fd, kNewData, sizeof(kNewData), 0);
  const int close_error = CloseChecked(fd);
  if (error) {
    return error;
  }
  return close_error ? close_error : EIO;
#else
  // The selected interruption is inside the replacement-rename transaction.
  // A successful return would mean the test hook was not reached.
  return rename(kSourcePath, kDestinationPath) == 0 ? EIO : ErrorOrEIO();
#endif
}

static int CommitPostRecoveryMutation(void) {
  int error = CreateFile(kPostRecoveryPath, kPostRecoveryData,
                         sizeof(kPostRecoveryData), NULL);
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int VerifyPostRecoveryMutation(void) {
  return CheckFile(kPostRecoveryPath, kPostRecoveryData,
                   sizeof(kPostRecoveryData), 0);
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend);
  int role = kSeed;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_SEED)
  if (!error) {
    error = SeedFilesystem();
  }
  role = kSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPTOR)
  if (!error) {
    error = VerifySelectedState(false);
  }
  if (!error) {
    error = RunInterruptedMutation();
  }
  role = kInterruptor;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_VERIFIER)
  if (!error) {
    error = VerifySelectedState(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_EXPECT_NEW);
  }
  if (!error) {
    error = FlushDirectory();
  }
  if (!error) {
    error = CommitPostRecoveryMutation();
  }
  role = kVerifier;
#else
  if (!error) {
    error = VerifySelectedState(
      WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_EXPECT_NEW);
  }
  if (!error) {
    error = FlushDirectory();
  }
  if (!error) {
    error = VerifyPostRecoveryMutation();
  }
  role = kReload;
#endif

  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(role, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
