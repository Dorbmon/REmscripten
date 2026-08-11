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
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kPath[] = "/opfs/wasmfs-opfs-positioned-io-range";
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

int main(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory("/opfs", 0777, backend) == 0);

  // The test may be rerun against the same OPFS origin after an interrupted
  // browser run.
  (void)unlink(kPath);
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  const size_t size = sizeof(kContents) - 1;
  assert(pwrite(fd, kContents, size, 0) == (ssize_t)size);

  const off_t maxOffset = (off_t)LLONG_MAX;
  char scalarWrite[] = {'S', 'C'};
  char first = 'A';
  char second = 'B';
  struct iovec writeIovs[] = {{.iov_base = &first, .iov_len = 1},
                              {.iov_base = &second, .iov_len = 1}};

  // These must be rejected in WasmFS before the OPFS bridge receives an
  // unrepresentable endpoint or can modify the baseline file.
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
  errno = 0;
  expectFailure(pread(fd, scalar, sizeof(scalar), maxOffset - 1), EINVAL);
  errno = 0;
  expectFailure(preadv(fd, readIovs, 2, maxOffset - 1), EINVAL);
  assert(scalar[0] == 's');
  assert(scalar[1] == 'c');
  assert(vectorFirst == 'x');
  assert(vectorSecond == 'y');
  assertContents(fd);

  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);
  return 0;
}
