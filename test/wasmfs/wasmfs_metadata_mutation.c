/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void assertMode(int fd, mode_t expected) {
  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
  assert((stat_buf.st_mode & 0777) == expected);
}

static void assertTimes(int fd, const struct timespec expected[2]) {
  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
  assert(stat_buf.st_atim.tv_sec == expected[0].tv_sec);
  assert(stat_buf.st_atim.tv_nsec == expected[0].tv_nsec);
  assert(stat_buf.st_mtim.tv_sec == expected[1].tv_sec);
  assert(stat_buf.st_mtim.tv_nsec == expected[1].tv_nsec);
}

int main(void) {
  static const char kPath[] = "wasmfs-metadata-mutation";
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);

  assert(fchmodat(AT_FDCWD, kPath, 0640, 0) == 0);
  assertMode(fd, 0640);
  assert(fchmod(fd, 0600) == 0);
  assertMode(fd, 0600);

  const struct timespec path_times[2] = {
    {42, 111000000},
    {43, 222000000},
  };
  assert(utimensat(AT_FDCWD, kPath, path_times, 0) == 0);
  assertTimes(fd, path_times);

  const struct timespec fd_times[2] = {
    {44, 333000000},
    {45, 444000000},
  };
  assert(futimens(fd, fd_times) == 0);
  assertTimes(fd, fd_times);

  const struct timespec omit_times[2] = {
    {0, UTIME_OMIT},
    {0, UTIME_OMIT},
  };
  assert(utimensat(AT_FDCWD, kPath, omit_times, 0) == 0);
  assertTimes(fd, fd_times);
  assert(futimens(fd, omit_times) == 0);
  assertTimes(fd, fd_times);

  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);
  puts("ok");
  return 0;
}
