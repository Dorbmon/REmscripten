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

static const char kPath[] = "wasmfs-positioned-io-range";
static const char kContents[] = "preserve";

static void expectFailure(ssize_t result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

static void assertContents(int fd) {
  const size_t size = sizeof(kContents) - 1;
  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
  assert(stat_buf.st_size == (off_t)size);

  char actual[sizeof(kContents)] = {};
  assert(pread(fd, actual, size, 0) == (ssize_t)size);
  assert(memcmp(actual, kContents, size) == 0);
}

static void testRangeFailures(int fd) {
  const off_t maxOffset = (off_t)LLONG_MAX;
  char scalarWrite[] = {'S', 'C'};
  char first = 'A';
  char second = 'B';
  struct iovec writeIovs[] = {{.iov_base = &first, .iov_len = 1},
                              {.iov_base = &second, .iov_len = 1}};

  // Neither a scalar crossing nor one that only crosses after the first iovec
  // may reach the data backend or grow the file.
  errno = 0;
  expectFailure(
    pwrite(fd, scalarWrite, sizeof(scalarWrite), maxOffset - 1), EINVAL);
  errno = 0;
  expectFailure(pwritev(fd, writeIovs, 2, maxOffset - 1), EINVAL);
  assertContents(fd);

  char scalar[] = {'s', 'c'};
  char vectorFirst = 'x';
  char vectorSecond = 'y';
  struct iovec readIovs[] = {{&vectorFirst, 1}, {&vectorSecond, 1}};

  // Failed positioned reads must not let a backend overwrite any destination
  // buffer before the whole vector range has been validated.
  errno = 0;
  expectFailure(pread(fd, scalar, sizeof(scalar), maxOffset - 1), EINVAL);
  assert(scalar[0] == 's');
  assert(scalar[1] == 'c');
  errno = 0;
  expectFailure(preadv(fd, readIovs, 2, maxOffset - 1), EINVAL);
  assert(vectorFirst == 'x');
  assert(vectorSecond == 'y');
}

static void testPrecedence(int fd) {
  const off_t maxOffset = (off_t)LLONG_MAX;
  const char marker = 'M';
  char buffer = 0;

  // The descriptor-table lookup remains first, even for an invalid signed
  // offset at the WASI ABI boundary.
  errno = 0;
  expectFailure(pwrite(-1, &marker, 1, -1), EBADF);
  errno = 0;
  expectFailure(pread(-1, &buffer, 1, -1), EBADF);

  int readOnly = open(kPath, O_RDONLY);
  assert(readOnly >= 0);
  errno = 0;
  expectFailure(pwrite(readOnly, &marker, 1, -1), EINVAL);
  errno = 0;
  expectFailure(pwrite(readOnly, &marker, 1, maxOffset), EBADF);
  assert(close(readOnly) == 0);

  int writeOnly = open(kPath, O_WRONLY);
  assert(writeOnly >= 0);
  errno = 0;
  expectFailure(pread(writeOnly, &buffer, 1, -1), EINVAL);
  errno = 0;
  expectFailure(pread(writeOnly, &buffer, 1, maxOffset), EBADF);
  assert(close(writeOnly) == 0);

  assert(mkdir("wasmfs-positioned-io-range-dir", 0700) == 0);
  int directory =
    open("wasmfs-positioned-io-range-dir", O_RDONLY | O_DIRECTORY);
  assert(directory >= 0);
  errno = 0;
  expectFailure(pread(directory, &buffer, 1, maxOffset), EISDIR);
  assert(close(directory) == 0);
  assert(rmdir("wasmfs-positioned-io-range-dir") == 0);

  // Keep the writable descriptor live through the end of this helper so the
  // caller can recheck the original file contents afterwards.
  assert(fd >= 0);
}

static void testPipes(void) {
  int pipefd[2];
  assert(pipe(pipefd) == 0);

  char marker = 'P';
  const char expected = 'Q';
  assert(write(pipefd[1], &expected, 1) == 1);
  errno = 0;
  expectFailure(pread(pipefd[0], &marker, 1, 0), ESPIPE);
  errno = 0;
  expectFailure(pwrite(pipefd[1], &marker, 1, 0), ESPIPE);

  // Seekability is checked before descriptor access mode for valid positioned
  // offsets, while an invalid signed offset still wins first.
  errno = 0;
  expectFailure(pread(pipefd[1], &marker, 1, 0), ESPIPE);
  errno = 0;
  expectFailure(pwrite(pipefd[0], &marker, 1, 0), ESPIPE);
  errno = 0;
  expectFailure(pread(pipefd[0], &marker, 1, -1), EINVAL);
  errno = 0;
  expectFailure(pwrite(pipefd[1], &marker, 1, -1), EINVAL);

  // Neither failed positioned operation may consume or append pipe data.
  char actual = 0;
  assert(read(pipefd[0], &actual, 1) == 1);
  assert(actual == expected);

  assert(close(pipefd[0]) == 0);
  assert(close(pipefd[1]) == 0);
}

int main(void) {
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  const size_t size = sizeof(kContents) - 1;
  assert(pwrite(fd, kContents, size, 0) == (ssize_t)size);

  testRangeFailures(fd);
  testPrecedence(fd);
  assertContents(fd);
  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);

  testPipes();
  puts("ok");
  return 0;
}
