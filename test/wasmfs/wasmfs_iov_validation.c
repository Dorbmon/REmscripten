/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static const char kPath[] = "wasmfs-iov-validation";
static const char kDirectory[] = "wasmfs-iov-validation-dir";
static const char kContents[] = "preserve";

static void expectFailure(ssize_t result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

static void expectPosition(int fd, off_t expected) {
  assert(lseek(fd, 0, SEEK_CUR) == expected);
}

static void assertContents(int fd) {
  const size_t size = sizeof(kContents) - 1;
  char actual[sizeof(kContents)] = {};
  assert(pread(fd, actual, size, 0) == (ssize_t)size);
  assert(memcmp(actual, kContents, size) == 0);
}

static void testNullVector(int fd) {
  const off_t position = 3;
  assert(lseek(fd, position, SEEK_SET) == position);

  errno = 0;
  expectFailure(readv(fd, NULL, 1), EFAULT);
  expectPosition(fd, position);
  errno = 0;
  expectFailure(writev(fd, NULL, 1), EFAULT);
  expectPosition(fd, position);
  errno = 0;
  expectFailure(preadv(fd, NULL, 1, 0), EFAULT);
  expectPosition(fd, position);
  errno = 0;
  expectFailure(pwritev(fd, NULL, 1, 0), EFAULT);
  expectPosition(fd, position);
  assertContents(fd);

  // A null vector is valid with no entries, including for positioned calls.
  assert(readv(fd, NULL, 0) == 0);
  assert(writev(fd, NULL, 0) == 0);
  assert(preadv(fd, NULL, 0, 0) == 0);
  assert(pwritev(fd, NULL, 0, 0) == 0);
  expectPosition(fd, position);
  assertContents(fd);
}

static void testCountValidation(int fd) {
  struct iovec tooMany[UIO_MAXIOV + 1] = {};

  // Count validation must win before dereferencing a nonempty vector.
  errno = 0;
  expectFailure(readv(fd, NULL, UIO_MAXIOV + 1), EINVAL);
  errno = 0;
  expectFailure(pwritev(fd, NULL, -1, 0), EINVAL);

  errno = 0;
  expectFailure(readv(fd, tooMany, UIO_MAXIOV + 1), EINVAL);
  errno = 0;
  expectFailure(writev(fd, tooMany, UIO_MAXIOV + 1), EINVAL);
  errno = 0;
  expectFailure(preadv(fd, tooMany, UIO_MAXIOV + 1, 0), EINVAL);
  errno = 0;
  expectFailure(pwritev(fd, tooMany, UIO_MAXIOV + 1, 0), EINVAL);

  errno = 0;
  expectFailure(readv(fd, tooMany, -1), EINVAL);
  errno = 0;
  expectFailure(writev(fd, tooMany, -1), EINVAL);
  errno = 0;
  expectFailure(preadv(fd, tooMany, -1, 0), EINVAL);
  errno = 0;
  expectFailure(pwritev(fd, tooMany, -1, 0), EINVAL);
}

static void testAggregateLengthValidation(int fd) {
  char byte = 0;
  struct iovec iovs[] = {{.iov_base = &byte, .iov_len = INT_MAX},
                         {.iov_base = &byte, .iov_len = INT_MAX}};
  const off_t position = 123;
  assert(lseek(fd, position, SEEK_SET) == position);

  // /dev/null makes the pre-fix large write harmless, while still exposing
  // the wrapped success result and open-file-position update.
  errno = 0;
  expectFailure(writev(fd, iovs, 2), EINVAL);
  expectPosition(fd, position);
  errno = 0;
  expectFailure(readv(fd, iovs, 2), EINVAL);
  expectPosition(fd, position);
  errno = 0;
  expectFailure(pwritev(fd, iovs, 2, 0), EINVAL);
  expectPosition(fd, position);
  errno = 0;
  expectFailure(preadv(fd, iovs, 2, 0), EINVAL);
  expectPosition(fd, position);
}

static void testPrecedence(int fd) {
  char byte = 0;
  struct iovec valid = {.iov_base = &byte, .iov_len = 1};

  // Descriptor lookup wins even over an invalid positioned offset and a null
  // vector.
  errno = 0;
  expectFailure(readv(-1, NULL, 1), EBADF);
  errno = 0;
  expectFailure(writev(-1, NULL, 1), EBADF);
  errno = 0;
  expectFailure(preadv(-1, NULL, 1, -1), EBADF);
  errno = 0;
  expectFailure(pwritev(-1, NULL, 1, -1), EBADF);

  // A positioned offset is checked before a null vector.
  errno = 0;
  expectFailure(preadv(fd, NULL, 1, -1), EINVAL);
  errno = 0;
  expectFailure(pwritev(fd, NULL, 1, -1), EINVAL);

  int pipefd[2];
  assert(pipe(pipefd) == 0);
  // Positioned pipe operations reject the non-seekable descriptor before the
  // vector and before access mode.
  errno = 0;
  expectFailure(preadv(pipefd[0], NULL, 1, 0), ESPIPE);
  errno = 0;
  expectFailure(preadv(pipefd[1], NULL, 1, 0), ESPIPE);
  errno = 0;
  expectFailure(pwritev(pipefd[0], NULL, 1, 0), ESPIPE);
  errno = 0;
  expectFailure(pwritev(pipefd[1], NULL, 1, 0), ESPIPE);
  assert(close(pipefd[0]) == 0);
  assert(close(pipefd[1]) == 0);

  int readOnly = open(kPath, O_RDONLY);
  assert(readOnly >= 0);
  errno = 0;
  expectFailure(writev(readOnly, NULL, 1), EBADF);
  errno = 0;
  expectFailure(pwritev(readOnly, NULL, 1, 0), EBADF);
  assert(close(readOnly) == 0);

  int writeOnly = open(kPath, O_WRONLY);
  assert(writeOnly >= 0);
  errno = 0;
  expectFailure(readv(writeOnly, NULL, 1), EBADF);
  errno = 0;
  expectFailure(preadv(writeOnly, NULL, 1, 0), EBADF);
  assert(close(writeOnly) == 0);

  assert(mkdir(kDirectory, 0700) == 0);
  int directory = open(kDirectory, O_RDONLY | O_DIRECTORY);
  assert(directory >= 0);
  // A valid nonempty vector reaches the directory check. Malformed vectors
  // are rejected first, and an empty vector succeeds without requiring data
  // file operations.
  errno = 0;
  expectFailure(readv(directory, &valid, 1), EISDIR);
  errno = 0;
  expectFailure(preadv(directory, &valid, 1, 0), EISDIR);
  errno = 0;
  expectFailure(readv(directory, NULL, 1), EFAULT);
  errno = 0;
  expectFailure(preadv(directory, NULL, 1, 0), EFAULT);
  assert(readv(directory, NULL, 0) == 0);
  assert(preadv(directory, NULL, 0, 0) == 0);
  assert(close(directory) == 0);
  assert(rmdir(kDirectory) == 0);
}

int main(void) {
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  const size_t size = sizeof(kContents) - 1;
  assert(pwrite(fd, kContents, size, 0) == (ssize_t)size);

  testNullVector(fd);
  testPrecedence(fd);
  assertContents(fd);
  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);

  int nullfd = open("/dev/null", O_RDWR);
  assert(nullfd >= 0);
  testCountValidation(nullfd);
  testAggregateLengthValidation(nullfd);
  assert(close(nullfd) == 0);

  puts("ok");
  return 0;
}
