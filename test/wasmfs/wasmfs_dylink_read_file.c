/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  See the LICENSE file for
 * details.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <emscripten/emscripten.h>

EM_JS(int, exerciseDylinkReadFile, (int expectedErrno), {
  const path = 'wasmfs-dylink-read-file';
  if (FS.readFile(path, {encoding: 'utf8'}) !== 'abc') {
    return 1;
  }

  let error;
  try {
    FS.readFile(path, {flags: 'w', encoding: 'utf8'});
  } catch (e) {
    error = e;
  }
  return error && error.name === 'ErrnoError' && error.errno === expectedErrno ?
             0 :
             2;
});

int main(void) {
  int fd = open("wasmfs-dylink-read-file", O_CREAT | O_TRUNC | O_WRONLY, 0666);
  assert(fd >= 0);
  assert(write(fd, "abc", 3) == 3);
  assert(close(fd) == 0);
  assert(exerciseDylinkReadFile(EBADF) == 0);
  puts("ok");
  return 0;
}
