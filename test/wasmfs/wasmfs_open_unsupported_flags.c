/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void expectRejectedOpen(const char* path, int flag, int expectedErrno) {
  errno = 0;
  assert(open(path, O_CREAT | O_EXCL | O_RDWR | flag, 0600) == -1);
  assert(errno == expectedErrno);

  // A rejected O_CREAT request must not leave a side effect.
  errno = 0;
  assert(access(path, F_OK) == -1);
  assert(errno == ENOENT);
}

static void expectAllowedOpen(const char* path, int flags) {
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR | flags, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);
  assert(unlink(path) == 0);
}

int main(void) {
  expectRejectedOpen("direct-open", O_DIRECT, ENOTSUP);
  expectRejectedOpen("tmpfile-open", O_TMPFILE, ENOTSUP);
  expectRejectedOpen(
    "partial-tmpfile-open", O_TMPFILE & ~O_DIRECTORY, EINVAL);
  expectRejectedOpen("async-open", O_ASYNC, ENOTSUP);
  expectRejectedOpen("noatime-open", O_NOATIME, ENOTSUP);
  expectRejectedOpen("unknown-open", 1 << 30, EINVAL);

  errno = 0;
  assert(open(".", O_PATH) == -1);
  assert(errno == EINVAL);

  int dirfd = open(".", O_RDONLY | O_DIRECTORY);
  assert(dirfd >= 0);
  assert(close(dirfd) == 0);

  assert(mkdir("read-only-parent", 0555) == 0);
  expectRejectedOpen("read-only-parent/new", 0, EACCES);
  assert(chmod("read-only-parent", 0755) == 0);
  assert(rmdir("read-only-parent") == 0);

  expectAllowedOpen("cloexec-nonblock-open", O_CLOEXEC | O_NONBLOCK);
  expectAllowedOpen("noctty-open", O_NOCTTY);

  puts("ok");
  return 0;
}
