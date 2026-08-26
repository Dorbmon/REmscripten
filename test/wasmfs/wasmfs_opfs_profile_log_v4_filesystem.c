// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Browser-coordinated proof for the mountable V4 logical-profile filesystem.
// Every role runs in a fresh same-origin document and reaches OPFS only through
// the native leased backend.  The test proves durable directory topology,
// regular-file content and metadata, copy-on-write chunk reads, truncation
// and zero fill, replacement rename, symlink identity, unlinked-open-file
// behavior, directory fsync, and a clean profile-lease handoff.  It does not
// claim physical power-loss behavior, database recovery, Chromium shutdown
// integration, or record-lock support; those are separate gates.

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

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_MUTATOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_VERIFIER)
#error "select one V4 profile-log filesystem test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_MUTATOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_VERIFIER) != 1
#error "select exactly one V4 profile-log filesystem test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_PROFILE_NAME
#error "select a V4 profile-log filesystem test profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_STRINGIFY_IMPL(value)

enum TestRole {
  kOwner,
  kMutator,
  kVerifier,
};

enum {
  kChunkSize = 64 * 1024,
  kInitialSize = 3 * kChunkSize + 257,
  kGrowSize = 4 * kChunkSize + 513,
  kFinalSize = kChunkSize + 333,
  kPatchOffset = kChunkSize + 17,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs";
static const char kLivePath[] = "/v4fs/live";
static const char kIncomingPath[] = "/v4fs/incoming";
static const char kDataPath[] = "/v4fs/live/data";
static const char kLinkPath[] = "/v4fs/live/data-link";
static const char kReplacementPath[] = "/v4fs/live/replaced";
static const char kMovePath[] = "/v4fs/incoming/move-me";
static const char kOrphanPath[] = "/v4fs/live/orphan";
static const char kStatePath[] = "/v4fs/state";
static const uint8_t kReplacement[] = "old replacement";
static const uint8_t kMove[] = "moved inode";
static const uint8_t kOrphan[] = "retained descriptor";
static const uint8_t kPatch[] = {
  0x42, 0x19, 0xa7, 0x5d, 0x3c, 0x81, 0xee, 0x27,
  0x90, 0x64, 0x1b, 0xd2, 0x78, 0x4f, 0xb5, 0x0c,
};
static const struct timespec kOwnerTimes[2] = {
  {42, 111000000},
  {43, 222000000},
};
static uint8_t payload[kGrowSize];

struct Witness {
  uint8_t magic[8];
  uint32_t schema;
  uint32_t phase;
  uint64_t data_ino;
  uint64_t move_ino;
  uint64_t replaced_ino;
  uint64_t orphan_ino;
  uint64_t link_ino;
  uint64_t state_ino;
  uint64_t data_size;
  int64_t data_mtime_sec;
  int64_t data_mtime_nsec;
};

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

static void MakeInitialPayload(void) {
  for (size_t index = 0; index != kInitialSize; ++index) {
    payload[index] = (uint8_t)(0x31 + index * 29 + (index >> 7) * 17);
  }
}

static void MakeFinalPayload(void) {
  MakeInitialPayload();
  memcpy(payload + kPatchOffset, kPatch, sizeof(kPatch));
}

static int WriteExact(int fd,
                      const void* bytes,
                      size_t size,
                      off_t offset) {
  const ssize_t written = pwrite(fd, bytes, size, offset);
  return written == (ssize_t)size ? 0 : ErrorOrEIO();
}

static int ReadExact(int fd, void* bytes, size_t size, off_t offset) {
  const ssize_t read_size = pread(fd, bytes, size, offset);
  return read_size == (ssize_t)size ? 0 : ErrorOrEIO();
}

static int CloseChecked(int fd) {
  return close(fd) == 0 ? 0 : ErrorOrEIO();
}

static int FlushDirectory(const char* path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = fsync(fd) == 0 ? 0 : ErrorOrEIO();
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int FlushAllDirectories(void) {
  int error = FlushDirectory(kIncomingPath);
  if (!error) {
    error = FlushDirectory(kLivePath);
  }
  if (!error) {
    error = FlushDirectory(kMountPath);
  }
  return error;
}

static int MountFilesystem(backend_t* backend) {
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

static int CheckOpaqueBoundary(backend_t backend) {
  uint8_t byte = 0;
  size_t size = 0;
  return wasmfs_opfs_profile_log_v4_read_manifest(
           backend, &byte, sizeof(byte), &size) == -ENOTSUP &&
         wasmfs_opfs_profile_log_v4_commit_manifest(
           backend, &byte, sizeof(byte)) == -ENOTSUP
           ? 0
           : EIO;
}

static int CheckAbsent(const char* path);

static int CheckMountBoundary(backend_t backend) {
  // A V4 filesystem factory supplies exactly one mount root. A second mount
  // must fail before it can publish a cache-only alias to the same persistent
  // inode tree.
  if (wasmfs_create_directory("/v4fs-alias", 0700, backend) >= 0 ||
      CheckAbsent("/v4fs-alias")) {
    return EIO;
  }

  // The historic cross-backend wasmfs_create_file() route is likewise a
  // cache-only mount. The transactional profile backend rejects it instead of
  // treating an inode-0 private creation candidate as a successful file.
  if (wasmfs_create_file("/v4fs-alias-file", 0600, backend) != -ENOTSUP ||
      CheckAbsent("/v4fs-alias-file")) {
    return EIO;
  }
  return 0;
}

static int WriteSmallFile(const char* path,
                          const uint8_t* bytes,
                          size_t size) {
  const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, bytes, size, 0);
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int ReadWitness(struct Witness* witness) {
  if (!witness) {
    return EINVAL;
  }
  const int fd = open(kStatePath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = ReadExact(fd, witness, sizeof(*witness), 0);
  const int close_error = CloseChecked(fd);
  if (error) {
    return error;
  }
  if (close_error) {
    return close_error;
  }
  return memcmp(witness->magic, "WFSV4FS1", sizeof(witness->magic)) == 0 &&
         witness->schema == 1 && witness->state_ino && witness->data_ino &&
         witness->move_ino && witness->replaced_ino && witness->orphan_ino &&
         witness->link_ino
           ? 0
           : EIO;
}

static int WriteWitness(const struct Witness* witness) {
  const int fd = open(kStatePath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, witness, sizeof(*witness), 0);
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int CheckDataStat(const struct Witness* witness,
                         off_t expected_size,
                         int check_owner_mtime) {
  struct stat status = {};
  if (stat(kDataPath, &status) != 0) {
    return ErrorOrEIO();
  }
  if (!S_ISREG(status.st_mode) || (status.st_mode & S_IALLUGO) != 0640 ||
      (uint64_t)status.st_ino != witness->data_ino ||
      status.st_size != expected_size || status.st_ctim.tv_sec <= 0) {
    return EIO;
  }
  if (check_owner_mtime &&
      (status.st_mtim.tv_sec != kOwnerTimes[1].tv_sec ||
       status.st_mtim.tv_nsec != kOwnerTimes[1].tv_nsec)) {
    return EIO;
  }
  if (!check_owner_mtime &&
      (status.st_mtim.tv_sec != witness->data_mtime_sec ||
       status.st_mtim.tv_nsec != witness->data_mtime_nsec)) {
    return EIO;
  }
  return 0;
}

static int CheckLink(const struct Witness* witness) {
  struct stat status = {};
  if (lstat(kLinkPath, &status) != 0 || !S_ISLNK(status.st_mode) ||
      (uint64_t)status.st_ino != witness->link_ino) {
    return EIO;
  }
  char target[16] = {};
  const ssize_t target_size = readlink(kLinkPath, target, sizeof(target));
  if (target_size != 4 || memcmp(target, "data", 4) != 0) {
    return ErrorOrEIO();
  }
  const int fd = open(kLinkPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  uint8_t byte = 0;
  int error = ReadExact(fd, &byte, sizeof(byte), 0);
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }
  MakeInitialPayload();
  return byte == payload[0] ? 0 : EIO;
}

static int CheckAbsent(const char* path) {
  struct stat status = {};
  errno = 0;
  return lstat(path, &status) == -1 && errno == ENOENT ? 0 : EIO;
}

static int PopulateOwner(struct Witness* witness) {
  if (mkdir(kLivePath, 0700) != 0 || mkdir(kIncomingPath, 0700) != 0) {
    return ErrorOrEIO();
  }

  int fd = open(kDataPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  MakeInitialPayload();
  int error = WriteExact(fd, payload, kInitialSize, 0);
  if (!error && fchmod(fd, 0640) != 0) {
    error = ErrorOrEIO();
  }
  if (!error && futimens(fd, kOwnerTimes) != 0) {
    error = ErrorOrEIO();
  }
  struct stat data_status = {};
  if (!error && fstat(fd, &data_status) != 0) {
    error = ErrorOrEIO();
  }
  if (!error && (!S_ISREG(data_status.st_mode) ||
                 (data_status.st_mode & S_IALLUGO) != 0640 ||
                 data_status.st_size != kInitialSize || !data_status.st_ino ||
                 data_status.st_mtim.tv_sec != kOwnerTimes[1].tv_sec ||
                 data_status.st_mtim.tv_nsec != kOwnerTimes[1].tv_nsec)) {
    error = EIO;
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }

  if ((error = WriteSmallFile(kReplacementPath, kReplacement,
                              sizeof(kReplacement) - 1)) ||
      (error = WriteSmallFile(kMovePath, kMove, sizeof(kMove) - 1)) ||
      (error = WriteSmallFile(kOrphanPath, kOrphan, sizeof(kOrphan) - 1))) {
    return error;
  }
  if (symlink("data", kLinkPath) != 0) {
    return ErrorOrEIO();
  }

  memset(witness, 0, sizeof(*witness));
  memcpy(witness->magic, "WFSV4FS1", sizeof(witness->magic));
  witness->schema = 1;
  witness->phase = 1;
  witness->data_ino = data_status.st_ino;
  witness->data_size = kInitialSize;
  witness->data_mtime_sec = data_status.st_mtim.tv_sec;
  witness->data_mtime_nsec = data_status.st_mtim.tv_nsec;
  struct stat status = {};
  if (stat(kMovePath, &status) != 0 || !status.st_ino) {
    return ErrorOrEIO();
  }
  witness->move_ino = status.st_ino;
  if (stat(kReplacementPath, &status) != 0 || !status.st_ino) {
    return ErrorOrEIO();
  }
  witness->replaced_ino = status.st_ino;
  if (stat(kOrphanPath, &status) != 0 || !status.st_ino) {
    return ErrorOrEIO();
  }
  witness->orphan_ino = status.st_ino;
  if (lstat(kLinkPath, &status) != 0 || !S_ISLNK(status.st_mode) ||
      !status.st_ino) {
    return ErrorOrEIO();
  }
  witness->link_ino = status.st_ino;

  fd = open(kStatePath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  error = WriteExact(fd, witness, sizeof(*witness), 0);
  if (!error && fstat(fd, &status) != 0) {
    error = ErrorOrEIO();
  }
  if (!error) {
    witness->state_ino = status.st_ino;
    error = WriteExact(fd, witness, sizeof(*witness), 0);
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int state_close_error = CloseChecked(fd);
  if (error || state_close_error) {
    return error ? error : state_close_error;
  }
  return FlushAllDirectories();
}

static int VerifyOwner(const struct Witness* witness) {
  if (!witness || witness->phase != 1 ||
      CheckDataStat(witness, kInitialSize, true) || CheckLink(witness)) {
    return EIO;
  }
  struct stat status = {};
  if (stat(kMovePath, &status) != 0 ||
      (uint64_t)status.st_ino != witness->move_ino ||
      stat(kReplacementPath, &status) != 0 ||
      (uint64_t)status.st_ino != witness->replaced_ino ||
      stat(kOrphanPath, &status) != 0 ||
      (uint64_t)status.st_ino != witness->orphan_ino) {
    return EIO;
  }
  const int fd = open(kDataPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  MakeInitialPayload();
  static uint8_t actual[kInitialSize];
  int error = ReadExact(fd, actual, sizeof(actual), 0);
  if (!error && memcmp(actual, payload, sizeof(actual)) != 0) {
    error = EIO;
  }
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int Mutate(struct Witness* witness) {
  int fd = open(kDataPath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = ftruncate(fd, kGrowSize) == 0 ? 0 : ErrorOrEIO();
  static uint8_t zeros[kChunkSize];
  if (!error && ReadExact(fd, zeros, kChunkSize, kInitialSize) != 0) {
    error = ErrorOrEIO();
  }
  if (!error) {
    for (size_t index = 0; index != sizeof(zeros); ++index) {
      if (zeros[index] != 0) {
        error = EIO;
        break;
      }
    }
  }
  if (!error) {
    error = WriteExact(fd, kPatch, sizeof(kPatch), kPatchOffset);
  }
  if (!error) {
    error = WriteExact(fd, kPatch, sizeof(kPatch), kInitialSize + 31);
  }
  if (!error && ftruncate(fd, kFinalSize) != 0) {
    error = ErrorOrEIO();
  }
  struct stat status = {};
  if (!error && fstat(fd, &status) != 0) {
    error = ErrorOrEIO();
  }
  if (!error && (status.st_size != kFinalSize ||
                 (uint64_t)status.st_ino != witness->data_ino)) {
    error = EIO;
  }
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }

  struct stat source = {};
  struct stat replaced = {};
  if (stat(kMovePath, &source) != 0 || stat(kReplacementPath, &replaced) != 0 ||
      (uint64_t)source.st_ino != witness->move_ino ||
      (uint64_t)replaced.st_ino != witness->replaced_ino) {
    return ErrorOrEIO();
  }
  if (rename(kMovePath, kReplacementPath) != 0) {
    return ErrorOrEIO();
  }
  if (CheckAbsent(kMovePath) || stat(kReplacementPath, &status) != 0 ||
      (uint64_t)status.st_ino != witness->move_ino) {
    return EIO;
  }

  fd = open(kOrphanPath, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  if (unlink(kOrphanPath) != 0) {
    error = ErrorOrEIO();
  }
  uint8_t orphan[sizeof(kOrphan) - 1] = {};
  if (!error) {
    error = ReadExact(fd, orphan, sizeof(orphan), 0);
  }
  if (!error && memcmp(orphan, kOrphan, sizeof(orphan)) != 0) {
    error = EIO;
  }
  if (!error && CheckAbsent(kOrphanPath)) {
    error = EIO;
  }
  const int orphan_close_error = CloseChecked(fd);
  if (error || orphan_close_error) {
    return error ? error : orphan_close_error;
  }

  if (stat(kDataPath, &status) != 0 ||
      (uint64_t)status.st_ino != witness->data_ino ||
      status.st_size != kFinalSize) {
    return ErrorOrEIO();
  }
  witness->phase = 2;
  witness->data_size = status.st_size;
  witness->data_mtime_sec = status.st_mtim.tv_sec;
  witness->data_mtime_nsec = status.st_mtim.tv_nsec;
  if ((error = WriteWitness(witness))) {
    return error;
  }
  return FlushAllDirectories();
}

static int VerifyFinal(const struct Witness* witness) {
  if (!witness || witness->phase != 2 || witness->data_size != kFinalSize ||
      CheckDataStat(witness, kFinalSize, false) || CheckLink(witness)) {
    return EIO;
  }
  static uint8_t actual[kFinalSize];
  const int fd = open(kDataPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = ReadExact(fd, actual, sizeof(actual), 0);
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }
  MakeFinalPayload();
  if (memcmp(actual, payload, sizeof(actual)) != 0) {
    return EIO;
  }
  struct stat status = {};
  if (stat(kReplacementPath, &status) != 0 ||
      (uint64_t)status.st_ino != witness->move_ino ||
      CheckAbsent(kMovePath) || CheckAbsent(kOrphanPath)) {
    return EIO;
  }
  return FlushAllDirectories();
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  struct Witness witness = {};
  int error = MountFilesystem(&backend);

  if (!error) {
    error = CheckOpaqueBoundary(backend);
  }
  if (!error) {
    error = CheckMountBoundary(backend);
  }

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_OWNER)
  if (!error) {
    error = PopulateOwner(&witness);
  }
  const int role = kOwner;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_MUTATOR)
  if (!error) {
    error = ReadWitness(&witness);
  }
  if (!error) {
    error = VerifyOwner(&witness);
  }
  if (!error) {
    error = Mutate(&witness);
  }
  const int role = kMutator;
#else
  if (!error) {
    error = ReadWitness(&witness);
  }
  if (!error) {
    error = VerifyFinal(&witness);
  }
  const int role = kVerifier;
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
