// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static void checkRelativeFile(const char* name, char contents) {
  int fd = open(name, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(fd >= 0);
  assert(write(fd, &contents, 1) == 1);
  assert(lseek(fd, 0, SEEK_SET) == 0);
  char actual = 0;
  assert(read(fd, &actual, 1) == 1);
  assert(actual == contents);
  assert(close(fd) == 0);
}

static void expectRmdirBusy(const char* path) {
  errno = 0;
  assert(rmdir(path) == -1);
  assert(errno == EBUSY);
}

int main(void) {
  backend_t backend = wasmfs_create_memory_backend();
  assert(backend);

  assert(wasmfs_create_directory("/mount", 0777, backend) == 0);
  assert(chdir("/mount") == 0);
  assert(wasmfs_unmount("/mount") == -EBUSY);
  checkRelativeFile("at-mount", 'M');
  assert(chdir("/") == 0);
  assert(wasmfs_unmount("/mount") == 0);

  assert(wasmfs_create_directory("/mount-child", 0777, backend) == 0);
  assert(mkdir("/mount-child/child", 0777) == 0);
  assert(chdir("/mount-child/child") == 0);
  assert(wasmfs_unmount("/mount-child") == -EBUSY);
  checkRelativeFile("below-mount", 'C');
  assert(chdir("/") == 0);
  assert(wasmfs_unmount("/mount-child") == 0);

  assert(wasmfs_create_directory("/mount-fd", 0777, backend) == 0);
  int dirfd = open("/mount-fd", O_RDONLY | O_DIRECTORY);
  assert(dirfd >= 0);
  assert(fchdir(dirfd) == 0);
  assert(wasmfs_unmount("/mount-fd") == -EBUSY);
  checkRelativeFile("at-fd-mount", 'F');
  assert(chdir("/") == 0);
  assert(close(dirfd) == 0);
  assert(wasmfs_unmount("/mount-fd") == 0);

  assert(wasmfs_create_directory("/rmdir-mount", 0777, backend) == 0);
  expectRmdirBusy("/rmdir-mount");
  assert(wasmfs_unmount("/rmdir-mount") == 0);

  assert(wasmfs_create_directory("/rename-source", 0777, backend) == 0);
  assert(wasmfs_create_directory("/rename-target", 0777, backend) == 0);
  assert(mkdir("/rename-source/cwd", 0777) == 0);
  assert(chdir("/rename-source/cwd") == 0);
  assert(rename("/rename-source/cwd", "/rename-target/cwd") == 0);
  assert(wasmfs_unmount("/rename-target") == -EBUSY);
  checkRelativeFile("after-rename", 'R');
  assert(chdir("/") == 0);
  assert(wasmfs_unmount("/rename-source") == 0);
  assert(wasmfs_unmount("/rename-target") == 0);

  puts("ok");
  return 0;
}
