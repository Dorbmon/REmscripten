/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char kPath[] = "wasmfs-ftruncate-descriptor-mode";
static const char kCreateModePath[] =
  "wasmfs-ftruncate-descriptor-create-mode";
static const char kContents[] = "contents";

static void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

static void assertContents(int fd, size_t expectedSize) {
  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
  assert(stat_buf.st_size == (off_t)expectedSize);

  char actual[sizeof(kContents)] = {};
  assert(pread(fd, actual, expectedSize, 0) == (ssize_t)expectedSize);
  assert(memcmp(actual, kContents, expectedSize) == 0);
}

int main(void) {
  const size_t originalSize = sizeof(kContents) - 1;

  errno = 0;
  // Descriptor lookup takes precedence over validation of the requested size.
  expectFailure(ftruncate(-1, -1), EBADF);

  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(pwrite(fd, kContents, originalSize, 0) == (ssize_t)originalSize);

  // A read-only descriptor remains invalid even while another writable
  // descriptor refers to the same file.
  int readOnly = open(kPath, O_RDONLY);
  assert(readOnly >= 0);
  errno = 0;
  expectFailure(ftruncate(readOnly, 3), EINVAL);
  assertContents(fd, originalSize);
  assert(close(readOnly) == 0);

  errno = 0;
  expectFailure(ftruncate(fd, -1), EINVAL);
  assertContents(fd, originalSize);

  // Pathname truncation checks the current logical file mode, whereas this
  // open file description remains writable after chmod removes those bits.
  assert(fchmod(fd, 0000) == 0);
  errno = 0;
  expectFailure(truncate(kPath, 3), EACCES);
  assertContents(fd, originalSize);

  assert(ftruncate(fd, 3) == 0);
  assertContents(fd, 3);
  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);

  // A descriptor's write access also authorizes ftruncate on a freshly
  // created file whose logical mode has no write bits.
  fd = open(kCreateModePath, O_CREAT | O_EXCL | O_WRONLY, 0000);
  assert(fd >= 0);
  assert(write(fd, kContents, originalSize) == (ssize_t)originalSize);
  assert(ftruncate(fd, 3) == 0);
  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
  assert(stat_buf.st_size == 3);
  assert(close(fd) == 0);
  assert(unlink(kCreateModePath) == 0);

  int pipefd[2];
  assert(pipe(pipefd) == 0);
  assert(write(pipefd[1], kContents, originalSize) ==
         (ssize_t)originalSize);
  errno = 0;
  expectFailure(ftruncate(pipefd[1], 0), EINVAL);

  char actual[sizeof(kContents)] = {};
  assert(read(pipefd[0], actual, originalSize) == (ssize_t)originalSize);
  assert(memcmp(actual, kContents, originalSize) == 0);
  assert(close(pipefd[0]) == 0);
  assert(close(pipefd[1]) == 0);

  puts("ok");
  return 0;
}
