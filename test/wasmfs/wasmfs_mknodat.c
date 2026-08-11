/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <emscripten/emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const dev_t kIgnoredDev = 123;

EM_JS(int,
      expectJSMknodFailure,
      (const char* path, int mode, int dev, int expectedErrno), {
  try {
    FS.mknod(UTF8ToString(path), mode, dev);
  } catch (e) {
    return e.name === 'ErrnoError' && e.errno === expectedErrno ? 0 : -1;
  }
  return -1;
});

static void expectNoNode(const char* path) {
  struct stat stat_buf;
  errno = 0;
  assert(lstat(path, &stat_buf) == -1);
  assert(errno == ENOENT);
}

static void expectFailureAt(const char* path,
                            mode_t mode,
                            int expectedErrno) {
  errno = 0;
  assert(mknodat(AT_FDCWD, path, mode, kIgnoredDev) == -1);
  assert(errno == expectedErrno);
  expectNoNode(path);
}

static void expectRegularAt(const char* path, mode_t mode) {
  assert(mknodat(AT_FDCWD, path, mode, kIgnoredDev) == 0);
  struct stat stat_buf;
  assert(stat(path, &stat_buf) == 0);
  assert(S_ISREG(stat_buf.st_mode));
  assert(unlink(path) == 0);
}

static void expectRegular(const char* path, mode_t mode) {
  assert(mknod(path, mode, kIgnoredDev) == 0);
  struct stat stat_buf;
  assert(stat(path, &stat_buf) == 0);
  assert(S_ISREG(stat_buf.st_mode));
  assert(unlink(path) == 0);
}

static void expectJSFailure(const char* path,
                            mode_t mode,
                            int expectedErrno) {
  assert(expectJSMknodFailure(path, mode, kIgnoredDev, expectedErrno) == 0);
  expectNoNode(path);
}

int main(void) {
  // A missing parent must not hide the unsupported node-type error.
  expectFailureAt("mknodat-missing-parent/character",
                  S_IFCHR | 0600,
                  ENOTSUP);

  // A zero type and an explicit regular-file type create ordinary files. The
  // nonzero device number is immaterial for both supported forms.
  expectRegularAt("mknodat-zero-type", 0600);
  expectRegular("mknodat-regular-type", S_IFREG | 0600);

  expectFailureAt("mknodat-character", S_IFCHR | 0600, ENOTSUP);
  expectFailureAt("mknodat-block", S_IFBLK | 0600, ENOTSUP);
  expectFailureAt("mknodat-fifo", S_IFIFO | 0600, ENOTSUP);
  expectFailureAt("mknodat-socket", S_IFSOCK | 0600, ENOTSUP);

  // The public WasmFS JS API must not recreate the prior release-only fake
  // success for special nodes.
  expectJSFailure("mknodat-js-character", S_IFCHR | 0600, ENOTSUP);

  expectFailureAt("mknodat-directory", S_IFDIR | 0600, EINVAL);
  expectFailureAt("mknodat-link", S_IFLNK | 0600, EINVAL);
  expectFailureAt("mknodat-unknown", S_IFMT | 0600, EINVAL);

  puts("ok");
  return 0;
}
