/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kPath[] = "/opfs/wasmfs-opfs-fallocate";
static const char kContents[] = "preserve";

int main(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory("/opfs", 0777, backend) == 0);

  // The test may be rerun against the same OPFS origin after an interrupted
  // browser run.
  (void)unlink(kPath);
  // OPFS cannot persist POSIX mode bits. The writable descriptor, rather than
  // this newly-created wrapper's logical mode, authorizes fallocate.
  int fd = open(kPath, O_CREAT | O_EXCL | O_RDWR, 0000);
  assert(fd >= 0);

  const size_t original_size = sizeof(kContents) - 1;
  assert(pwrite(fd, kContents, original_size, 0) == (ssize_t)original_size);

#if defined(WASMFS_OPFS_FALLOCATE_INJECTED)
  // The hook fails before the native SyncAccessHandle truncate, so allocation
  // must report the quota error and leave the existing data alone.
  assert(posix_fallocate(fd, original_size, 5) == ENOSPC);
#else
  assert(posix_fallocate(fd, original_size, 5) == 0);
#endif

  struct stat stat_buf;
  assert(fstat(fd, &stat_buf) == 0);
#if defined(WASMFS_OPFS_FALLOCATE_INJECTED)
  assert(stat_buf.st_size == (off_t)original_size);
  char actual[sizeof(kContents)] = {};
  assert(pread(fd, actual, original_size, 0) == (ssize_t)original_size);
  assert(memcmp(actual, kContents, original_size) == 0);
#else
  const size_t allocated_size = original_size + 5;
  assert(stat_buf.st_size == (off_t)allocated_size);
  char expected[sizeof(kContents) + 5] = {};
  memcpy(expected, kContents, original_size);
  char actual[sizeof(expected)] = {};
  assert(pread(fd, actual, allocated_size, 0) == (ssize_t)allocated_size);
  assert(memcmp(actual, expected, allocated_size) == 0);
#endif

  // A quota failure before truncate does not poison the live access handle.
  assert(fdatasync(fd) == 0);
  assert(close(fd) == 0);
  assert(unlink(kPath) == 0);
  return 0;
}
