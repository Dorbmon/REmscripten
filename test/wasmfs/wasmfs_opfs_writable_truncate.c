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

static const char kPath[] = "/opfs/wasmfs-opfs-writable-truncate";
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

  // This pathname operation takes the OPFS FileSystemWritableFileStream path,
  // not the open-file SyncAccessHandle path covered by ftruncate.
#if defined(WASMFS_OPFS_WRITABLE_TRUNCATE_INJECTED)
  errno = 0;
  assert(truncate(kPath, 3) == -1);
  assert(errno == ENOSPC);
#else
  assert(truncate(kPath, 3) == 0);
#endif

  // In the injected case, aborting the failed writable stream must release
  // its write lock before this fresh writable open can succeed. The normal
  // path also verifies that close releases the stream lock.
  int reopened = open(kPath, O_RDWR);
  assert(reopened >= 0);
  assert(close(reopened) == 0);

  struct stat stat_buf;
  assert(stat(kPath, &stat_buf) == 0);
#if defined(WASMFS_OPFS_WRITABLE_TRUNCATE_INJECTED)
  // The hook fires before native truncate, so the original file must remain
  // untouched. This is a controlled pre-mutation test, not an atomicity claim.
  assert(stat_buf.st_size == (off_t)original_size);
#else
  assert(stat_buf.st_size == 3);
#endif

  fd = open(kPath, O_RDONLY);
  assert(fd >= 0);
#if defined(WASMFS_OPFS_WRITABLE_TRUNCATE_INJECTED)
  char actual[sizeof(kContents)] = {};
  assert(pread(fd, actual, original_size, 0) == (ssize_t)original_size);
  assert(memcmp(actual, kContents, original_size) == 0);
#else
  char actual[4] = {};
  assert(pread(fd, actual, 3, 0) == 3);
  assert(memcmp(actual, kContents, 3) == 0);
#endif
  assert(close(fd) == 0);

  assert(unlink(kPath) == 0);
  return 0;
}
