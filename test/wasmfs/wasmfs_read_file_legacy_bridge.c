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

EM_JS(int, exerciseLegacyReadFileBridge, (int expectedErrno), {
  const path = 'wasmfs-read-file-legacy-bridge';
  if (FS.readFile(path, {encoding: 'utf8'}) !== 'abc') {
    return 1;
  }

  for (let i = 0; i < 2; ++i) {
    let error;
    try {
      FS.readFile(path, {flags: 'w', encoding: 'utf8'});
    } catch (e) {
      error = e;
    }
    if (!error || error.name !== 'ErrnoError' || error.errno !== expectedErrno) {
      return 2 + i;
    }
  }

  return FS.readFile(path, {encoding: 'utf8'}) === 'abc' ? 0 : 4;
});

int main(void) {
  int fd = open("wasmfs-read-file-legacy-bridge",
                O_CREAT | O_TRUNC | O_WRONLY, 0666);
  assert(fd >= 0);
  assert(write(fd, "abc", 3) == 3);
  assert(close(fd) == 0);
  assert(exerciseLegacyReadFileBridge(ENOTSUP) == 0);
  puts("ok");
  return 0;
}
