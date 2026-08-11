/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <emscripten/syscalls.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char kPath[] = "wasmfs-fallocate";
static const char kContents[] = "0123456789";

static void assertFile(int fd, const char* expected, size_t expectedSize) {
  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
  assert(stat_buf.st_size == (off_t)expectedSize);

  char actual[sizeof(kContents) + 16] = {};
  assert(expectedSize <= sizeof(actual));
  assert(pread(fd, actual, expectedSize, 0) == (ssize_t)expectedSize);
  assert(memcmp(actual, expected, expectedSize) == 0);
}

int main(void) {
  const size_t originalSize = sizeof(kContents) - 1;
  char expected[sizeof(kContents) + 16] = {};
  memcpy(expected, kContents, originalSize);

  // Descriptor lookup takes precedence over flag and range validation.
  assert(__syscall_fallocate(-1, FALLOC_FL_KEEP_SIZE, 0, 1) == -EBADF);

  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(pwrite(fd, kContents, originalSize, 0) == (ssize_t)originalSize);

  // The POSIX wrapper exercises the ordinary mode-0 allocation path.
  assert(posix_fallocate(fd, originalSize, 5) == 0);
  const size_t allocatedSize = originalSize + 5;
  assertFile(fd, expected, allocatedSize);

  int readOnly = open(kPath, O_RDONLY);
  assert(readOnly >= 0);
  // posix_fallocate returns its error number directly rather than setting
  // errno. Its failed request must not grow the file.
  assert(posix_fallocate(readOnly, allocatedSize, 1) == EBADF);
  assertFile(fd, expected, allocatedSize);
  assert(close(readOnly) == 0);

  assert(__syscall_fallocate(fd, 0, -1, 1) == -EINVAL);
  assert(__syscall_fallocate(fd, 0, 0, 0) == -EINVAL);
  assertFile(fd, expected, allocatedSize);

  // KEEP_SIZE and PUNCH_HOLE need storage semantics that DataFile::setSize
  // cannot provide. They must not silently act like mode zero.
  assert(__syscall_fallocate(fd, FALLOC_FL_KEEP_SIZE, allocatedSize, 5) ==
         -ENOTSUP);
  assert(__syscall_fallocate(
           fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, 2, 4) ==
         -ENOTSUP);
  assertFile(fd, expected, allocatedSize);

  assert(__syscall_fallocate(fd, 0, (off_t)LLONG_MAX, 1) == -EFBIG);
  assertFile(fd, expected, allocatedSize);

  // Like ftruncate, fallocate is authorized by the writable open file
  // description, not mode bits that can change after open.
  assert(fchmod(fd, 0000) == 0);
  assert(posix_fallocate(fd, allocatedSize, 5) == 0);
  const size_t descriptorAuthorizedSize = allocatedSize + 5;
  assertFile(fd, expected, descriptorAuthorizedSize);

  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);

  int pipefd[2];
  assert(pipe(pipefd) == 0);
  assert(write(pipefd[1], kContents, originalSize) == (ssize_t)originalSize);
  // This range is already within the pipe's current queue length, proving the
  // syscall rejects a nonseekable descriptor before its size fast path.
  assert(posix_fallocate(pipefd[1], 0, 1) == ESPIPE);
  char actual[sizeof(kContents)] = {};
  assert(read(pipefd[0], actual, originalSize) == (ssize_t)originalSize);
  assert(memcmp(actual, kContents, originalSize) == 0);
  assert(close(pipefd[0]) == 0);
  assert(close(pipefd[1]) == 0);

  puts("ok");
  return 0;
}
