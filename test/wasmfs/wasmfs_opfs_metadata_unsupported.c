/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kMountPath[] = "/opfs";
static const char kPath[] = "/opfs/wasmfs-opfs-metadata-unsupported";
static const char kMissingPath[] = "/opfs/wasmfs-opfs-metadata-missing";
static const char kDirectoryPath[] =
  "/opfs/wasmfs-opfs-metadata-unsupported-dir";

static void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

static void assertSameMetadata(const struct stat* actual,
                               const struct stat* expected) {
  assert(actual->st_mode == expected->st_mode);
  assert(actual->st_atim.tv_sec == expected->st_atim.tv_sec);
  assert(actual->st_atim.tv_nsec == expected->st_atim.tv_nsec);
  assert(actual->st_mtim.tv_sec == expected->st_mtim.tv_sec);
  assert(actual->st_mtim.tv_nsec == expected->st_mtim.tv_nsec);
  assert(actual->st_ctim.tv_sec == expected->st_ctim.tv_sec);
  assert(actual->st_ctim.tv_nsec == expected->st_ctim.tv_nsec);
}

static void assertUnchanged(int fd, const struct stat* expected) {
  struct stat actual;
  assert(fstat(fd, &actual) == 0);
  assertSameMetadata(&actual, expected);
}

static void assertPathUnchanged(const char* path,
                                const struct stat* expected) {
  struct stat actual;
  assert(stat(path, &actual) == 0);
  assertSameMetadata(&actual, expected);
}

int main(void) {
  backend_t opfs_backend = wasmfs_create_opfs_backend();
  assert(opfs_backend);

  // The virtual backend must preserve OPFS's explicit-metadata capability.
  backend_t backend = wasmfs_create_icase_backend(opfs_backend);
  assert(backend);
  assert(wasmfs_create_directory(kMountPath, 0777, backend) == 0);

  // The test may be rerun against the same OPFS origin after an interrupted
  // browser run.
  errno = 0;
  if (unlink(kPath) != 0) {
    assert(errno == ENOENT);
  }
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);

  struct stat before;
  assert(fstat(fd, &before) == 0);
  const struct timespec changed_times[2] = {
    {42, 111000000},
    {43, 222000000},
  };
  const struct timespec omit_times[2] = {
    {0, UTIME_OMIT},
    {0, UTIME_OMIT},
  };
  const struct timespec partially_omitted_times[2] = {
    {0, UTIME_OMIT},
    {43, 222000000},
  };

  // Validation and lookup errors take precedence over backend capability.
  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, kPath, 0644, AT_EMPTY_PATH), EINVAL);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, kMissingPath, 0644, 0), ENOENT);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchmod(-1, 0644), EBADF);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(
    utimensat(AT_FDCWD, kPath, changed_times, AT_EMPTY_PATH), EINVAL);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(utimensat(AT_FDCWD, kMissingPath, changed_times, 0), ENOENT);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(futimens(-1, changed_times), EBADF);
  assertUnchanged(fd, &before);

  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, kPath, 0644, 0), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchmod(fd, 0644), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(utimensat(AT_FDCWD, kPath, changed_times, 0), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(futimens(fd, changed_times), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(
    utimensat(AT_FDCWD, kPath, partially_omitted_times, 0), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(futimens(fd, partially_omitted_times), ENOTSUP);
  assertUnchanged(fd, &before);

  // With both values omitted, utimensat does not request a metadata mutation.
  assert(utimensat(AT_FDCWD, kPath, omit_times, 0) == 0);
  assertUnchanged(fd, &before);
  assert(futimens(fd, omit_times) == 0);
  assertUnchanged(fd, &before);

  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);

  errno = 0;
  if (rmdir(kDirectoryPath) != 0) {
    assert(errno == ENOENT);
  }
  assert(mkdir(kDirectoryPath, 0755) == 0);
  struct stat directory_before;
  assert(stat(kDirectoryPath, &directory_before) == 0);

  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, kDirectoryPath, 0700, 0), ENOTSUP);
  assertPathUnchanged(kDirectoryPath, &directory_before);
  errno = 0;
  expectFailure(
    utimensat(AT_FDCWD, kDirectoryPath, changed_times, 0), ENOTSUP);
  assertPathUnchanged(kDirectoryPath, &directory_before);
  assert(utimensat(AT_FDCWD, kDirectoryPath, omit_times, 0) == 0);
  assertPathUnchanged(kDirectoryPath, &directory_before);

  assert(rmdir(kDirectoryPath) == 0);
  return 0;
}
