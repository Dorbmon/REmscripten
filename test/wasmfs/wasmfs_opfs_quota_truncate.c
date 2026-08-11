// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kPath[] = "/opfs/wasmfs-opfs-quota-truncate";
static const char kContents[] = "preserve";

int main(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory("/opfs", 0777, backend) == 0);

  // The test may be rerun against the same OPFS origin after an interrupted
  // browser run.
  (void)unlink(kPath);
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);

  const size_t original_size = sizeof(kContents) - 1;
  assert(pwrite(fd, kContents, original_size, 0) == (ssize_t)original_size);

#if defined(WASMFS_OPFS_QUOTA_TRUNCATE_INJECTED)
  errno = 0;
  assert(ftruncate(fd, 3) == -1);
  assert(errno == ENOSPC);
#else
  assert(ftruncate(fd, 3) == 0);
#endif

  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
#if defined(WASMFS_OPFS_QUOTA_TRUNCATE_INJECTED)
  // The injected QuotaExceededError occurs before the browser truncate, so
  // the original size and contents must remain intact.
  assert(stat_buf.st_size == (off_t)original_size);
  char actual[sizeof(kContents)] = {};
  assert(pread(fd, actual, original_size, 0) == (ssize_t)original_size);
  assert(memcmp(actual, kContents, original_size) == 0);
#else
  // The default-off companion proves that a real SyncAccessHandle truncate
  // reaches the requested length and preserves its prefix.
  assert(stat_buf.st_size == 3);
  char actual[4] = {};
  assert(pread(fd, actual, 3, 0) == 3);
  assert(memcmp(actual, kContents, 3) == 0);
#endif

  // This injected pre-call truncate failure does not poison an otherwise
  // healthy access handle, and the normal exit reporter checks the close plus
  // WasmFS teardown.
  assert(fdatasync(fd) == 0);
  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);
  return 0;
}
