// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kPath[] = "/opfs/wasmfs-opfs-quota-write";

int main(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory("/opfs", 0777, backend) == 0);

  // The test may be rerun against the same OPFS origin after an interrupted
  // browser run.
  (void)unlink(kPath);
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);

  static const char marker = 'q';
#if defined(WASMFS_OPFS_QUOTA_WRITE_INJECTED)
  errno = 0;
  assert(pwrite(fd, &marker, 1, 0) == -1);
  assert(errno == ENOSPC);
#else
  assert(pwrite(fd, &marker, 1, 0) == 1);
#endif

  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
#if defined(WASMFS_OPFS_QUOTA_WRITE_INJECTED)
  // The injected QuotaExceededError occurs before the browser write, so an
  // access-handle size query must still observe the empty file.
  assert(stat_buf.st_size == 0);
#else
  assert(stat_buf.st_size == 1);
  char actual = 0;
  assert(pread(fd, &actual, 1, 0) == 1);
  assert(actual == marker);
#endif

  // A failed write does not poison an otherwise healthy access handle, and
  // the normal exit reporter checks the close plus WasmFS teardown.
  assert(fdatasync(fd) == 0);
  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);
  return 0;
}
