// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kMountPath[] = "/opfs";
static const char kFilePath[] = "/opfs/wasmfs-opfs-root-remount";

static void WriteAndSync(int fd, char marker) {
  assert(pwrite(fd, &marker, 1, 0) == 1);
  assert(fdatasync(fd) == 0);
}

int main(void) {
  // The backend and its ProxyWorker stay alive after wasmfs_unmount(). The
  // second mount must therefore reuse its permanent OPFS root handle.
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);

  assert(wasmfs_create_directory(kMountPath, 0777, backend) == 0);
  int fd = open(kFilePath, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(fd >= 0);
  WriteAndSync(fd, 'A');
  assert(close(fd) == 0);

  assert(wasmfs_unmount(kMountPath) == 0);

  assert(wasmfs_create_directory(kMountPath, 0777, backend) == 0);
  fd = open(kFilePath, O_RDWR);
  assert(fd >= 0);
  char marker = 0;
  assert(pread(fd, &marker, 1, 0) == 1);
  assert(marker == 'A');
  WriteAndSync(fd, 'B');
  assert(close(fd) == 0);

  assert(wasmfs_unmount(kMountPath) == 0);
  return 0;
}
