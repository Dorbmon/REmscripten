// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

// This private test-only symbol is compiled only with
// -sWASMFS_RECORD_LOCK_TEST=1. F_GETLK intentionally hides locks owned by the
// caller's process, so it cannot observe POSIX's any-descriptor-close rule.
extern int wasmfs_record_lock_count_for_testing(int fd);

static struct flock MakeLock(short type,
                             short whence,
                             off_t start,
                             off_t len) {
  return (struct flock){
      .l_type = type,
      .l_whence = whence,
      .l_start = start,
      .l_len = len,
      .l_pid = -1,
  };
}

static void ExpectFcntlError(int fd, int command, struct flock lock, int error) {
  errno = 0;
  assert(fcntl(fd, command, &lock) == -1);
  assert(errno == error);
}

static void ExpectUnsupportedFlock(int fd, int operation) {
  errno = 0;
  assert(flock(fd, operation) == -1);
  assert(errno == ENOTSUP || errno == EOPNOTSUPP);
}

static void TestValidRanges(int fd) {
  struct flock lock = MakeLock(F_WRLCK, SEEK_SET, 0, 4);
  assert(fcntl(fd, F_SETLK, &lock) == 0);
  assert(wasmfs_record_lock_count_for_testing(fd) == 1);

  // An adjacent finite lock followed by an end-of-file lock must normalize to
  // one unbounded range instead of preserving a stale finite end.
  lock = MakeLock(F_WRLCK, SEEK_SET, 4, 0);
  assert(fcntl(fd, F_SETLKW, &lock) == 0);
  assert(wasmfs_record_lock_count_for_testing(fd) == 1);

  // Unlocking a finite middle range splits an existing to-EOF range.
  lock = MakeLock(F_UNLCK, SEEK_SET, 2, 2);
  assert(fcntl(fd, F_SETLK, &lock) == 0);
  assert(wasmfs_record_lock_count_for_testing(fd) == 2);

  assert(lseek(fd, 8, SEEK_SET) == 8);
  lock = MakeLock(F_RDLCK, SEEK_CUR, -2, 2);
  assert(fcntl(fd, F_SETLK, &lock) == 0);

  // A negative length selects the range ending immediately before l_start.
  lock = MakeLock(F_RDLCK, SEEK_SET, 4, -2);
  assert(fcntl(fd, F_SETLK, &lock) == 0);

  lock = MakeLock(F_WRLCK, SEEK_END, -1, 1);
  assert(fcntl(fd, F_SETLKW, &lock) == 0);

  lock = MakeLock(F_WRLCK, SEEK_SET, 0, 1);
  lock.l_pid = -1;
  assert(fcntl(fd, F_GETLK, &lock) == 0);
  assert(lock.l_type == F_UNLCK);
  assert(lock.l_whence == SEEK_SET);
  assert(lock.l_start == 0);
  assert(lock.l_len == 1);
  assert(lock.l_pid == -1);
}

static void TestInvalidLocks(int fd) {
  errno = 0;
  assert(fcntl(fd, F_SETLK, NULL) == -1);
  assert(errno == EINVAL);
  ExpectFcntlError(
    fd, F_SETLK, MakeLock(99, SEEK_SET, 0, 1), EINVAL);
  ExpectFcntlError(
    fd, F_GETLK, MakeLock(F_UNLCK, SEEK_SET, 0, 1), EINVAL);
  ExpectFcntlError(
    fd, F_SETLK, MakeLock(F_WRLCK, 99, 0, 1), EINVAL);
  ExpectFcntlError(
    fd, F_GETLK, MakeLock(F_WRLCK, 99, 0, 1), EINVAL);
  ExpectFcntlError(
    fd, F_SETLK, MakeLock(F_WRLCK, SEEK_SET, -1, 1), EINVAL);
  ExpectFcntlError(
    fd, F_SETLK, MakeLock(F_WRLCK, SEEK_SET, 0, -1), EINVAL);
  ExpectFcntlError(
    fd, F_SETLK, MakeLock(F_WRLCK, SEEK_SET, LLONG_MAX, 1), EOVERFLOW);
  ExpectFcntlError(
    fd, F_GETLK, MakeLock(F_WRLCK, SEEK_SET, LLONG_MAX, 1), EOVERFLOW);
  assert(lseek(fd, 1, SEEK_SET) == 1);
  ExpectFcntlError(
    fd, F_SETLK, MakeLock(F_WRLCK, SEEK_CUR, LLONG_MAX, 0), EOVERFLOW);

  int directory = open(".", O_RDONLY | O_DIRECTORY);
  assert(directory >= 0);
  ExpectFcntlError(
    directory, F_SETLK, MakeLock(F_WRLCK, SEEK_SET, 0, 1), ENOTSUP);
  assert(close(directory) == 0);
}

static void TestAccessModes(const char* path) {
  int readonly = open(path, O_RDONLY);
  assert(readonly >= 0);
  ExpectFcntlError(
    readonly, F_SETLK, MakeLock(F_WRLCK, SEEK_SET, 0, 1), EBADF);
  assert(close(readonly) == 0);

  int writeonly = open(path, O_WRONLY);
  assert(writeonly >= 0);
  ExpectFcntlError(
    writeonly, F_SETLK, MakeLock(F_RDLCK, SEEK_SET, 0, 1), EBADF);
  assert(close(writeonly) == 0);
}

static void SetOneLock(int fd) {
  struct flock lock = MakeLock(F_WRLCK, SEEK_SET, 0, 1);
  assert(fcntl(fd, F_SETLK, &lock) == 0);
  assert(wasmfs_record_lock_count_for_testing(fd) == 1);
}

static void TestAnyCloseAndDup(const char* path, int fd) {
  SetOneLock(fd);
  int alias = dup(fd);
  assert(alias >= 0);
  assert(close(alias) == 0);
  assert(wasmfs_record_lock_count_for_testing(fd) == 0);

  SetOneLock(fd);
  int dup2_target = open(path, O_RDWR);
  assert(dup2_target >= 0);
  assert(dup2(fd, dup2_target) == dup2_target);
  assert(wasmfs_record_lock_count_for_testing(fd) == 0);

  SetOneLock(fd);
  int dup3_target = open(path, O_RDWR);
  assert(dup3_target >= 0);
  assert(dup3(fd, dup3_target, 0) == dup3_target);
  assert(wasmfs_record_lock_count_for_testing(fd) == 0);

  SetOneLock(fd);
  int fcntl_alias = fcntl(fd, F_DUPFD, 32);
  assert(fcntl_alias >= 32);
  assert(wasmfs_record_lock_count_for_testing(fd) == 1);
  assert(close(fcntl_alias) == 0);
  assert(wasmfs_record_lock_count_for_testing(fd) == 0);

  assert(close(dup2_target) == 0);
  assert(close(dup3_target) == 0);
}

int main(void) {
  const char path[] = "wasmfs_fcntl_locks";
  const int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  assert(fd >= 0);
  assert(pwrite(fd, "012345678", 9, 0) == 9);

  TestValidRanges(fd);
  TestInvalidLocks(fd);
  TestAccessModes(path);
  TestAnyCloseAndDup(path, fd);
  ExpectUnsupportedFlock(fd, LOCK_EX | LOCK_NB);
  ExpectUnsupportedFlock(fd, LOCK_UN);

  assert(close(fd) == 0);
  puts("success");
  return 0;
}
