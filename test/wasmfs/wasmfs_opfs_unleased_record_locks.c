// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <emscripten/wasmfs.h>
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
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory("/opfs-unleased-record-locks", 0777,
                                 backend) == 0);

  int fd = open("/opfs-unleased-record-locks/lock", O_CREAT | O_RDWR, 0600);
  assert(fd >= 0);
  ExpectUnsupported(fd, F_GETLK);
  ExpectUnsupported(fd, F_SETLK);
  ExpectUnsupported(fd, F_SETLKW);
  assert(close(fd) == 0);
  puts("success");
  return 0;
}
