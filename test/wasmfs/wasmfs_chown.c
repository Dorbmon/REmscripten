/*
 * Copyright 2022 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char kPath[] = "/working/foo.txt";
static const char kMissingPath[] = "/working/missing.txt";

static void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

static void assertSameMetadata(const struct stat* actual,
                               const struct stat* expected) {
  assert(actual->st_mode == expected->st_mode);
  assert(actual->st_uid == expected->st_uid);
  assert(actual->st_gid == expected->st_gid);
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

int main(void) {
  assert(mkdir("/working", 0777) == 0);

  int fd = creat(kPath, 0777);
  assert(fd >= 0);

  struct stat before;
  assert(fstat(fd, &before) == 0);

  // Validation and lookup errors take precedence over unsupported ownership
  // changes.
  errno = 0;
  expectFailure(fchownat(AT_FDCWD, kPath, 1, 1, AT_REMOVEDIR), EINVAL);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchownat(AT_FDCWD, kMissingPath, 1, 1, 0), ENOENT);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchown(-1, 1, 1), EBADF);
  assertUnchanged(fd, &before);
  // No-op arguments must not bypass validation or target lookup.
  errno = 0;
  expectFailure(fchownat(AT_FDCWD, kPath, -1, -1, AT_REMOVEDIR), EINVAL);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(chown(kMissingPath, -1, -1), ENOENT);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchown(-1, -1, -1), EBADF);
  assertUnchanged(fd, &before);

  // Each public wrapper fails closed when it would change ownership. Exercise
  // owner-only, group-only, and combined requests without changing metadata.
  errno = 0;
  expectFailure(fchown(fd, 1, -1), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(chown(kPath, -1, 1), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(lchown(kPath, 1, 1), ENOTSUP);
  assertUnchanged(fd, &before);
  errno = 0;
  expectFailure(fchownat(AT_FDCWD, kPath, -1, 1, 0), ENOTSUP);
  assertUnchanged(fd, &before);

  // A request to leave both values unchanged remains a successful no-op.
  assert(fchown(fd, -1, -1) == 0);
  assertUnchanged(fd, &before);
  assert(chown(kPath, -1, -1) == 0);
  assertUnchanged(fd, &before);
  assert(lchown(kPath, -1, -1) == 0);
  assertUnchanged(fd, &before);
  assert(fchownat(AT_FDCWD, kPath, -1, -1, 0) == 0);
  assertUnchanged(fd, &before);

  assert(close(fd) == 0);

  printf("ok\n");
}
