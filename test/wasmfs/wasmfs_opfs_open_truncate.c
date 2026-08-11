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

static const char kPath[] = "/opfs/wasmfs-opfs-open-truncate";
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
  assert(fdatasync(fd) == 0);
  assert(close(fd) == 0);

  // This is the open-file SyncAccessHandle path, unlike pathname truncate.
  // The injected build rejects the native truncate before it can mutate data.
  errno = 0;
  int truncating = open(kPath, O_WRONLY | O_TRUNC);
#if defined(WASMFS_OPFS_OPEN_TRUNCATE_INJECTED)
  assert(truncating == -1);
  assert(errno == ENOSPC);
#else
  assert(truncating >= 0);
  struct stat truncating_stat;
  assert(fstat(truncating, &truncating_stat) == 0);
  assert(truncating_stat.st_size == 0);
  assert(close(truncating) == 0);
#endif

  // Remove the cached file wrapper before reopening. The same backend must
  // retain its OPFS root across the remount, while a leaked browser-side access
  // handle from the rejected open would prevent this new wrapper from opening
  // the existing file for writing.
  assert(wasmfs_unmount("/opfs") == 0);
  assert(wasmfs_create_directory("/opfs", 0777, backend) == 0);

  // After either a normal close or a failed truncating open, a new writable
  // file wrapper must acquire an OPFS access handle. The injected case proves
  // that cleanup released the rejected open's handle before reporting ENOSPC.
  int reopened = open(kPath, O_RDWR);
  assert(reopened >= 0);

  struct stat stat_buf;
  assert(fstat(reopened, &stat_buf) == 0);
#if defined(WASMFS_OPFS_OPEN_TRUNCATE_INJECTED)
  assert(stat_buf.st_size == (off_t)original_size);
  char actual[sizeof(kContents)] = {};
  assert(pread(reopened, actual, original_size, 0) == (ssize_t)original_size);
  assert(memcmp(actual, kContents, original_size) == 0);
#else
  assert(stat_buf.st_size == 0);
#endif

  assert(close(reopened) == 0);
  assert(unlink(kPath) == 0);
  return 0;
}
