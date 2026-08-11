/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  See the LICENSE file for
 * details.
 */

#include <assert.h>
#include <emscripten/emscripten.h>
#include <errno.h>
#include <stdio.h>

EM_JS(int, exerciseMemory64Readdir, (int expectedErrno), {
  const entries = FS.readdir('.');
  if (!entries.includes('.') || !entries.includes('..')) {
    return 1;
  }

  let error;
  try {
    FS.readdir('wasmfs-readdir-memory64-missing');
  } catch (err) {
    error = err;
  }
  return error && error.name === 'ErrnoError' &&
                 error.errno === expectedErrno ? 0 : 2;
});

int main(void) {
  assert(exerciseMemory64Readdir(ENOENT) == 0);
  puts("ok");
  return 0;
}
