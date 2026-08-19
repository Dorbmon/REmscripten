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
  };
  errno = 0;
  assert(fcntl(fd, command, &lock) == -1);
  assert(errno == ENOTSUP || errno == EOPNOTSUPP);
}

int main(void) {
  int fd = open("wasmfs_fcntl_locks_unsupported", O_CREAT | O_RDWR | O_TRUNC,
                0600);
  assert(fd >= 0);
  ExpectUnsupported(fd, F_GETLK);
  ExpectUnsupported(fd, F_SETLK);
  ExpectUnsupported(fd, F_SETLKW);
  assert(close(fd) == 0);
  puts("success");
  return 0;
}
