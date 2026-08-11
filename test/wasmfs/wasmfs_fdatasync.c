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

int main() {
  int fd = creat("foo.txt", 0777);
  assert(fd > 0);

  errno = 0;
  assert(fsync(fd) == 0);
  assert(errno == 0);

  errno = 0;
  assert(fdatasync(fd) == 0);
  assert(errno == 0);

  close(fd);

  errno = 0;
  assert(fdatasync(fd) == -1);
  assert(errno == EBADF);

  int dirfd = open(".", O_RDONLY);
  assert(dirfd >= 0);

  errno = 0;
  assert(fsync(dirfd) == -1);
  assert(errno == ENOTSUP);

  errno = 0;
  assert(fdatasync(dirfd) == -1);
  assert(errno == ENOTSUP);

  assert(close(dirfd) == 0);

  printf("ok\n");
}
