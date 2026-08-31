// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Browser-coordinated proof that the opaque V4 manifest and mountable V4
// filesystem use distinct persistent physical namespaces for one logical
// profile name. Each role runs in a fresh same-origin document and reaches
// OPFS only through its native leased backend. The roles run sequentially
// because both formats intentionally share one profile lease; this is a
// format-isolation and orderly-persistence test, not a concurrent-writer,
// Chromium-profile, database-recovery, browser-crash, or power-loss claim.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_MUTATOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_VERIFIER)
#error "select one V4 profile-log format-namespace test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_MUTATOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_VERIFIER) != 1
#error "select exactly one V4 profile-log format-namespace test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_PROFILE_NAME
#error "select a V4 profile-log format-namespace test profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_STRINGIFY_IMPL(value)

enum TestRole {
  kManifestOwner,
  kFilesystemOwner,
  kManifestMutator,
  kFilesystemVerifier,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4-format";
static const char kSentinelPath[] = "/v4-format/sentinel";
static const uint8_t kManifest[] = {
  'V', '4', ' ', 'o', 'p', 'a', 'q', 'u', 'e', ' ', 'f', 'o', 'r', 'm', 'a',
  't', ' ', 'n', 'a', 'm', 'e', 's', 'p', 'a', 'c', 'e', ' ', 'p', 'a', 'y',
  'l', 'o', 'a', 'd', ':', 0x9d, 0x42, 0x17, 0xee, 0x63, 0x28, 0xb4,
};
static const uint8_t kUpdatedManifest[] = {
  'V', '4', ' ', 'o', 'p', 'a', 'q', 'u', 'e', ' ', 'f', 'o', 'r', 'm', 'a',
  't', ' ', 'm', 'u', 't', 'a', 't', 'i', 'o', 'n', ' ', 'p', 'a', 'y', 'l',
  'o', 'a', 'd', ':', 0x71, 0xc8, 0x2d, 0x94, 0x0b, 0xe6, 0x5a,
};
static const uint8_t kSentinel[] = {
  'V', '4', ' ', 'f', 'i', 'l', 'e', 's', 'y', 's', 't', 'e', 'm', ' ',
  'f', 'o', 'r', 'm', 'a', 't', ' ', 'n', 'a', 'm', 'e', 's', 'p', 'a',
  'c', 'e', ' ', 's', 'e', 'n', 't', 'i', 'n', 'e', 'l', ':', 0x6a, 0xf1,
  0x0c, 0x57, 0xa3, 0x89,
};
enum {
  kManifestBufferSize = sizeof(kManifest),
};

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static int ResultError(int result) { return result < 0 ? -result : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        event: 'result',
        result: 0,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-format-namespace',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

static int CloseChecked(int fd) {
  return close(fd) == 0 ? 0 : ErrorOrEIO();
}

static int WriteExact(int fd, const void* bytes, size_t size) {
  const ssize_t written = pwrite(fd, bytes, size, 0);
  return written == (ssize_t)size ? 0 : ErrorOrEIO();
}

static int ReadExact(int fd, void* bytes, size_t size) {
  const ssize_t read_size = pread(fd, bytes, size, 0);
  return read_size == (ssize_t)size ? 0 : ErrorOrEIO();
}

static int DrainBackend(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static int OpenManifest(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_manifest_backend(kProfileName);
  return *backend ? 0 : ErrorOrEIO();
}

static int CommitManifest(backend_t backend,
                          const uint8_t* bytes,
                          size_t size) {
  const int result = wasmfs_opfs_profile_log_v4_commit_manifest(
    backend, bytes, size);
  return result == 0 ? 0 : ResultError(result);
}

static int ReadManifest(backend_t backend,
                        const uint8_t* expected,
                        size_t expected_size) {
  size_t required = 0;
  int result = wasmfs_opfs_profile_log_v4_read_manifest(
    backend, NULL, 0, &required);
  if (result != 0 || required != expected_size) {
    return ResultError(result);
  }
  uint8_t actual[kManifestBufferSize] = {};
  if (expected_size > sizeof(actual)) {
    return EINVAL;
  }
  size_t size = 0;
  result = wasmfs_opfs_profile_log_v4_read_manifest(
    backend, actual, sizeof(actual), &size);
  return result == 0 && size == expected_size &&
           memcmp(actual, expected, expected_size) == 0
           ? 0
           : ResultError(result);
}

static int CheckManifestBoundary(backend_t backend) {
  return wasmfs_create_directory(
           "/v4-format-manifest-alias", 0700, backend) == -EIO
           ? 0
           : EIO;
}

static int FlushDirectory(const char* path) {
  const int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = fsync(fd) == 0 ? 0 : ErrorOrEIO();
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int MountFilesystem(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(kMountPath, 0700, *backend);
  return result == 0 ? 0 : ResultError(result);
}

static int CheckFilesystemBoundary(backend_t backend) {
  uint8_t byte = 0;
  size_t size = 0;
  return wasmfs_opfs_profile_log_v4_read_manifest(
           backend, &byte, sizeof(byte), &size) == -ENOTSUP &&
         wasmfs_opfs_profile_log_v4_commit_manifest(
           backend, &byte, sizeof(byte)) == -ENOTSUP
           ? 0
           : EIO;
}

static int WriteFilesystemSentinel(void) {
  const int fd = open(kSentinelPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = WriteExact(fd, kSentinel, sizeof(kSentinel));
  if (!error && fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  const int close_error = CloseChecked(fd);
  if (error || close_error) {
    return error ? error : close_error;
  }
  return FlushDirectory(kMountPath);
}

static int ReadFilesystemSentinel(void) {
  const int fd = open(kSentinelPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  uint8_t actual[sizeof(kSentinel)] = {};
  int error = ReadExact(fd, actual, sizeof(actual));
  struct stat status = {};
  if (!error && fstat(fd, &status) != 0) {
    error = ErrorOrEIO();
  }
  if (!error &&
      (!S_ISREG(status.st_mode) || status.st_size != sizeof(actual))) {
    error = EIO;
  }
  if (!error && memcmp(actual, kSentinel, sizeof(actual)) != 0) {
    error = EIO;
  }
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_OWNER)
  error = OpenManifest(&backend);
  if (!error) {
    error = CheckManifestBoundary(backend);
  }
  if (!error) {
    error = CommitManifest(backend, kManifest, sizeof(kManifest));
  }
  if (!error) {
    error = ReadManifest(backend, kManifest, sizeof(kManifest));
  }
  const int role = kManifestOwner;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_OWNER)
  error = MountFilesystem(&backend);
  if (!error) {
    error = CheckFilesystemBoundary(backend);
  }
  if (!error) {
    error = WriteFilesystemSentinel();
  }
  if (!error) {
    error = ReadFilesystemSentinel();
  }
  const int role = kFilesystemOwner;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_MUTATOR)
  error = OpenManifest(&backend);
  if (!error) {
    error = ReadManifest(backend, kManifest, sizeof(kManifest));
  }
  if (!error) {
    error = CommitManifest(
      backend, kUpdatedManifest, sizeof(kUpdatedManifest));
  }
  if (!error) {
    error = ReadManifest(
      backend, kUpdatedManifest, sizeof(kUpdatedManifest));
  }
  const int role = kManifestMutator;
#else
  error = MountFilesystem(&backend);
  if (!error) {
    error = ReadFilesystemSentinel();
  }
  if (!error) {
    error = FlushDirectory(kMountPath);
  }
  const int role = kFilesystemVerifier;
#endif

  if (backend) {
    const int drain_error = DrainBackend(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(role, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
