/*
 * Copyright 2011 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>

int main() {
  int f = open("test", O_RDWR, 0777);
  assert(f == 3);

  printf("F_DUPFD 1: %d\n", fcntl(f, F_DUPFD, 0) == 4);
  printf("errno: %d\n", errno);
  printf("\n");

  printf("F_DUPFD 2: %d\n", fcntl(f, F_DUPFD, 100) == 100);
  printf("errno: %d\n", errno);
  printf("\n");

  printf("F_DUPFD_CLOEXEC: %d\n", fcntl(f, F_DUPFD_CLOEXEC, 0) == 5);
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_DUPFD/error1: %d\n", fcntl(50, F_DUPFD, 200));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_DUPFD/error2: %d\n", fcntl(f, F_DUPFD, -1));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_GETFD: %d\n", fcntl(f, F_GETFD));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_SETFD: %d\n", fcntl(f, F_SETFD, -1));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_GETFL: %d\n", !!(fcntl(f, F_GETFL) & O_RDWR));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_SETFL: %d\n", fcntl(f, F_SETFL, O_APPEND));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_GETFL/2: %d\n", !!(fcntl(f, F_GETFL) & (O_RDWR | O_APPEND)));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  struct flock lk = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = 0,
      .l_len = 1,
  };
  int getlk = fcntl(f, F_GETLK, &lk);
  int getlk_errno = errno;
  printf("F_GETLK: %d\n", getlk);
  printf("errno: %d\n", getlk_errno);
  printf("lk.l_type == F_UNLCK: %d\n", lk.l_type == F_UNLCK);
  printf("\n");
  assert(getlk == -1);
  assert(getlk_errno == ENOTSUP);
  assert(lk.l_type == F_WRLCK);
  errno = 0;

  int err = fcntl(f, F_SETLK, &lk);
  assert(err == -1);
  assert(errno == ENOTSUP);

  errno = 0;
  err = fcntl(f, F_SETLKW, &lk);
  assert(err == -1);
  assert(errno == ENOTSUP);

  printf("F_SETOWN: %d\n", fcntl(f, F_SETOWN, 123));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("F_GETOWN: %d\n", fcntl(f, F_GETOWN));
  printf("errno: %d\n", errno);
  printf("\n");
  errno = 0;

  printf("INVALID: %d\n", fcntl(f, 123, -1));
  printf("errno: %d\n", errno);

  return 0;
}
