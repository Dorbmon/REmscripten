/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

// Verify that the file system enforces the open file access mode on read/write,
// as POSIX requires: writing to an O_RDONLY descriptor and reading from an
// O_WRONLY descriptor must fail with EBADF. This should behave identically
// across all of our filesystems (MEMFS, WASMFS, NODEFS, NODERAWFS).

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  const char* msg = "hello";
  const size_t len = strlen(msg);
  char buf[16];

  // Create a file with some content.
  int fd = open("testfile", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  assert(fd >= 0);
  assert(write(fd, msg, len) == (ssize_t)len);
  assert(close(fd) == 0);

#ifdef WASMFS
  // Descriptor access errors take priority over the file type. In particular,
  // writing through a read-only directory descriptor must fail with EBADF,
  // rather than the EISDIR file-type error.
  assert(mkdir("testdir", 0777) == 0);
  fd = open("testdir", O_RDONLY | O_DIRECTORY);
  assert(fd >= 0);
  errno = 0;
  assert(write(fd, msg, len) == -1);
  assert(errno == EBADF);
  assert(close(fd) == 0);
#endif

  // O_RDONLY: reads are allowed, writes must fail with EBADF.
  fd = open("testfile", O_RDONLY);
  assert(fd >= 0);

  errno = 0;
  assert(write(fd, msg, len) == -1);
  assert(errno == EBADF);

  errno = 0;
  assert(pwrite(fd, msg, len, 0) == -1);
  assert(errno == EBADF);

#ifdef WASMFS
  // ftruncate requires a descriptor that was opened for writing. Its failed
  // request must not change the file's size or contents.
  errno = 0;
  assert(ftruncate(fd, (off_t)len - 1) == -1);
  assert(errno == EINVAL);
  struct stat st;
  assert(fstat(fd, &st) == 0);
  assert(st.st_size == (off_t)len);
#endif

  errno = 0;
  memset(buf, 0, sizeof(buf));
  assert(read(fd, buf, sizeof(buf)) == (ssize_t)len);
  assert(errno == 0);
  assert(strcmp(buf, msg) == 0);
  assert(close(fd) == 0);

  // O_WRONLY: writes are allowed, reads must fail with EBADF.
  fd = open("testfile", O_WRONLY);
  assert(fd >= 0);

  errno = 0;
  assert(read(fd, buf, sizeof(buf)) == -1);
  assert(errno == EBADF);

  errno = 0;
  assert(pread(fd, buf, sizeof(buf), 0) == -1);
  assert(errno == EBADF);

#ifdef WASMFS
  // Invalid offsets take precedence over descriptor access errors.
  errno = 0;
  assert(pread(fd, buf, sizeof(buf), (off_t)-1) == -1);
  assert(errno == EINVAL);
#endif

  errno = 0;
  assert(write(fd, msg, len) == (ssize_t)len);
  assert(errno == 0);
  assert(close(fd) == 0);

  // O_RDWR: both reads and writes are allowed.
  fd = open("testfile", O_RDWR);
  assert(fd >= 0);

#ifdef WASMFS
  // F_SETFL may change O_APPEND, but must preserve O_ACCMODE. In particular,
  // clearing O_APPEND must not turn this descriptor into an O_RDONLY one.
  assert(fcntl(fd, F_SETFL, O_APPEND) == 0);
  int flags = fcntl(fd, F_GETFL);
  assert((flags & O_ACCMODE) == O_RDWR);
  assert(flags & O_APPEND);
  assert(write(fd, msg, len) == (ssize_t)len);

  assert(fcntl(fd, F_SETFL, 0) == 0);
  flags = fcntl(fd, F_GETFL);
  assert((flags & O_ACCMODE) == O_RDWR);
  assert(!(flags & O_APPEND));
#endif

  errno = 0;
  assert(lseek(fd, 0, SEEK_SET) == 0);
  memset(buf, 0, sizeof(buf));
  assert(read(fd, buf, sizeof(buf)) > 0);
  assert(write(fd, msg, len) == (ssize_t)len);
  assert(errno == 0);
  assert(close(fd) == 0);

  printf("done\n");
  return 0;
}
