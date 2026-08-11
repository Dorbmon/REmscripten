/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <emscripten/syscalls.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char kSourcePath[] = "dup3-flags-source";
static const char kTargetPath[] = "dup3-flags-target";
static const char kSourceContents[] = "source";
static const char kTargetContents[] = "target";

static void writeContents(int fd, const char* contents, size_t size) {
  assert(write(fd, contents, size) == (ssize_t)size);
}

static void expectContents(int fd, const char* contents, size_t size) {
  char buffer[sizeof(kSourceContents)];
  assert(size < sizeof(buffer));
  assert(lseek(fd, 0, SEEK_SET) == 0);
  assert(read(fd, buffer, size) == (ssize_t)size);
  assert(memcmp(buffer, contents, size) == 0);
}

static void expectRawRejectedFlags(int oldfd, int newfd, int flags) {
  assert(__syscall_dup3(oldfd, newfd, flags) == -EINVAL);
  expectContents(newfd, kTargetContents, sizeof(kTargetContents) - 1);
}

static void expectPublicRejectedFlags(int oldfd, int newfd, int flags) {
  errno = 0;
  assert(dup3(oldfd, newfd, flags) == -1);
  assert(errno == EINVAL);
  expectContents(newfd, kTargetContents, sizeof(kTargetContents) - 1);
}

int main(void) {
  assert(__syscall_dup3(-2, -2, 0) == -EINVAL);
  errno = 0;
  assert(dup3(-2, -2, 0) == -1);
  assert(errno == EINVAL);

  int source = open(kSourcePath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(source >= 0);
  int target = open(kTargetPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(target >= 0);
  writeContents(source, kSourceContents, sizeof(kSourceContents) - 1);
  writeContents(target, kTargetContents, sizeof(kTargetContents) - 1);

  assert(__syscall_dup3(source, source, 0) == -EINVAL);
  expectContents(source, kSourceContents, sizeof(kSourceContents) - 1);

  errno = 0;
  assert(dup3(source, source, 0) == -1);
  assert(errno == EINVAL);
  expectContents(source, kSourceContents, sizeof(kSourceContents) - 1);

  expectRawRejectedFlags(source, target, O_NONBLOCK);
  expectPublicRejectedFlags(source, target, O_CLOEXEC | O_NONBLOCK);

  assert(__syscall_dup3(source, target, 0) == target);
  expectContents(target, kSourceContents, sizeof(kSourceContents) - 1);
  assert(close(target) == 0);

  target = open(kTargetPath, O_RDWR);
  assert(target >= 0);
  expectContents(target, kTargetContents, sizeof(kTargetContents) - 1);
  assert(dup3(source, target, O_CLOEXEC) == target);
  expectContents(target, kSourceContents, sizeof(kSourceContents) - 1);
  assert(close(target) == 0);

  assert(close(source) == 0);
  assert(unlink(kSourcePath) == 0);
  assert(unlink(kTargetPath) == 0);
  puts("ok");
  return 0;
}
