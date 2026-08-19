// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void ExpectUnsupported(int fd, int command) {
  struct flock lock = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = 0,
      .l_len = 1,
      .l_pid = 123,
  };

  errno = 0;
  assert(fcntl(fd, command, &lock) == -1);
  assert(errno == ENOTSUP);
  // The unsupported JS syscall must not inspect or overwrite the flock.
  assert(lock.l_type == F_WRLCK);
  assert(lock.l_whence == SEEK_SET);
  assert(lock.l_start == 0);
  assert(lock.l_len == 1);
  assert(lock.l_pid == 123);
}

static void ExpectUnsupportedWithoutFlock(int fd) {
  // F_GETLK used to write F_UNLCK through this pointer. Every unsupported
  // record-lock command must return before inspecting or dereferencing it.
  const int commands[] = {F_GETLK, F_SETLK, F_SETLKW};
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    errno = 0;
    assert(fcntl(fd, commands[i], NULL) == -1);
    assert(errno == ENOTSUP);
  }
}

int main(void) {
#ifdef NO_FILESYSTEM
  // The no-filesystem implementation cannot look up an fd, but record lock
  // commands must still fail instead of reporting a synthetic success.
  int fd = STDOUT_FILENO;
#else
  int fd = open("legacy_fcntl_record_locks", O_CREAT | O_RDWR | O_TRUNC,
                0600);
  assert(fd >= 0);
#endif

  ExpectUnsupported(fd, F_GETLK);
  ExpectUnsupported(fd, F_SETLK);
  ExpectUnsupported(fd, F_SETLKW);
  ExpectUnsupportedWithoutFlock(fd);

#ifndef NO_FILESYSTEM
  assert(close(fd) == 0);
#endif
  puts("success");
  return 0;
}
