/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include <emscripten/emscripten.h>

// WASMFS_FD_MAX is an internal WasmFS implementation limit. Keep this test
// tied to the fixed table size without exposing it as a public API contract.
static const int kFailedReadCount = 4093;

EM_JS(int, exerciseReadFileCleanup, (int expectedErrno, int failedReadCount), {
  const path = 'wasmfs-read-file-cleanup';
  FS.writeFile(path, 'ok');

  // readFile opens the directory successfully, but the read itself fails with
  // EISDIR. Repeating it fills the descriptor table unless every failure closes
  // its descriptor.
  for (let i = 0; i < failedReadCount; ++i) {
    let error;
    try {
      FS.readFile('/');
    } catch (e) {
      error = e;
    }
    if (!error || error.name !== 'ErrnoError' || error.errno !== expectedErrno) {
      return 1;
    }
  }

  let stream;
  try {
    stream = FS.open(path, 'r');
    const contents = new Uint8Array(2);
    const bytesRead = FS.read(stream, contents, 0, contents.length, 0);
    FS.close(stream);
    stream = undefined;
    if (bytesRead !== contents.length || contents[0] !== 'o'.charCodeAt(0) ||
        contents[1] !== 'k'.charCodeAt(0)) {
      return 2;
    }
  } catch {
    if (stream) {
      try {
        FS.close(stream);
      } catch {}
    }
    return 3;
  }

  return 0;
});

EM_JS(int, exerciseReadFileBridge,
      (int pointerSize, int writeFlags, int expectedErrno), {
  const path = 'wasmfs-read-file-bridge';
  FS.writeFile(path, 'abc');

  let oldResult;
  let oldLength;
  let flagsResult;
  withStackSave(() => {
    const bufferPtr = stackAlloc(pointerSize);
    const sizePtr = stackAlloc(8);
    oldResult = __wasmfs_read_file(
      stringToUTF8OnStack(path), bufferPtr, sizePtr);
    if (oldResult === 0) {
      oldLength = readI53FromI64(sizePtr);
    }
    flagsResult = __wasmfs_read_file_with_flags(
      stringToUTF8OnStack(path), writeFlags, bufferPtr, sizePtr);
  });

  if (oldResult !== 0 || oldLength !== 3) {
    return 1;
  }
  if (flagsResult !== expectedErrno) {
    return 2;
  }
  if (FS.stat(path).size !== 0) {
    return 3;
  }
  FS.unlink(path);
  return 0;
});

int main(void) {
  assert(exerciseReadFileCleanup(EISDIR, kFailedReadCount) == 0);
  assert(exerciseReadFileBridge(sizeof(void*),
                                O_TRUNC | O_CREAT | O_WRONLY,
                                EBADF) == 0);
  puts("ok");
  return 0;
}
