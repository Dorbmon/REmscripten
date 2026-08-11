/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  See the LICENSE file for
 * details.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <emscripten/emscripten.h>

EM_JS(int, exerciseJsIoMemory,
      (int ebadf, int einval, int enomem, int espipe,
       int pipeReadFd, int pipeWriteFd), {
  function getError(operation) {
    try {
      operation();
    } catch (error) {
      return error;
    }
  }

  function hasErrno(error, errno) {
    return error && error.name === 'ErrnoError' && error.errno === errno;
  }

  function testInvalidArguments() {
    const oldMalloc = _malloc;
    const oldRead = __wasmfs_read;
    const oldWrite = __wasmfs_write;
    let mallocCalls = 0;
    let readCalls = 0;
    let writeCalls = 0;
    _malloc = function() {
      ++mallocCalls;
      return 1;
    };
    __wasmfs_read = function() {
      ++readCalls;
      return 0;
    };
    __wasmfs_write = function() {
      ++writeCalls;
      return 0;
    };
    try {
      const stream = {fd: 0};
      const data = new Uint8Array([0]);
      if (!hasErrno(getError(() => FS.read(stream, data, 0, -1)), einval) ||
          !hasErrno(getError(() => FS.write(stream, data, 0, -1)), einval) ||
          !hasErrno(getError(() => FS.read(stream, data, 0, 1, -1)), einval) ||
          !hasErrno(getError(() => FS.write(stream, data, 0, 1, -1)), einval)) {
        return false;
      }
      return mallocCalls === 0 && readCalls === 0 && writeCalls === 0;
    } finally {
      _malloc = oldMalloc;
      __wasmfs_read = oldRead;
      __wasmfs_write = oldWrite;
    }
  }

  function testZeroLengthValidation() {
    const oldMalloc = _malloc;
    const oldRead = __wasmfs_read;
    const oldWrite = __wasmfs_write;
    const oldPread = __wasmfs_pread;
    const oldPwrite = __wasmfs_pwrite;
    let mallocCalls = 0;
    let readCalls = 0;
    let writeCalls = 0;
    let preadCalls = 0;
    let pwriteCalls = 0;
    let zeroBuffers = true;
    _malloc = function() {
      ++mallocCalls;
      return 0;
    };
    __wasmfs_read = function(fd, buffer, length) {
      ++readCalls;
      zeroBuffers = zeroBuffers && buffer === 0;
      return oldRead(fd, buffer, length);
    };
    __wasmfs_write = function(fd, buffer, length) {
      ++writeCalls;
      zeroBuffers = zeroBuffers && buffer === 0;
      return oldWrite(fd, buffer, length);
    };
    __wasmfs_pread = function() {
      ++preadCalls;
      zeroBuffers = zeroBuffers && arguments[1] === 0;
      return oldPread.apply(null, arguments);
    };
    __wasmfs_pwrite = function() {
      ++pwriteCalls;
      zeroBuffers = zeroBuffers && arguments[1] === 0;
      return oldPwrite.apply(null, arguments);
    };
    try {
      const data = new Uint8Array(0);
      const readStream = {fd: pipeReadFd};
      const writeStream = {fd: pipeWriteFd};
      let readResult;
      let writeResult;
      return hasErrno(getError(() => FS.read({fd: -1}, data, 0, 0)), ebadf) &&
             hasErrno(getError(() => FS.write({fd: -1}, data, 0, 0)), ebadf) &&
             getError(() => { readResult = FS.read(readStream, data, 0, 0); }) === undefined &&
             getError(() => { writeResult = FS.write(writeStream, data, 0, 0); }) === undefined &&
             readResult === 0 && writeResult === 0 &&
             hasErrno(getError(() => FS.read(writeStream, data, 0, 0)), ebadf) &&
             hasErrno(getError(() => FS.write(readStream, data, 0, 0)), ebadf) &&
             hasErrno(getError(() => FS.read(readStream, data, 0, 0, 0)), espipe) &&
             hasErrno(getError(() => FS.write(writeStream, data, 0, 0, 0)), espipe) &&
             mallocCalls === 0 && zeroBuffers && readCalls === 3 &&
             writeCalls === 3 && preadCalls === 1 && pwriteCalls === 1;
    } finally {
      _malloc = oldMalloc;
      __wasmfs_read = oldRead;
      __wasmfs_write = oldWrite;
      __wasmfs_pread = oldPread;
      __wasmfs_pwrite = oldPwrite;
    }
  }

  function testMallocFailure() {
    const oldMalloc = _malloc;
    const oldRead = __wasmfs_read;
    const oldWrite = __wasmfs_write;
    let readCalls = 0;
    let writeCalls = 0;
    _malloc = function() { return 0; };
    __wasmfs_read = function() {
      ++readCalls;
      return 0;
    };
    __wasmfs_write = function() {
      ++writeCalls;
      return 0;
    };
    try {
      const stream = {fd: 0};
      return hasErrno(
               getError(() => FS.read(stream, new Uint8Array(1), 0, 1)), enomem) &&
             hasErrno(
               getError(() => FS.write(stream, new Uint8Array([0]), 0, 1)), enomem) &&
             readCalls === 0 && writeCalls === 0;
    } finally {
      _malloc = oldMalloc;
      __wasmfs_read = oldRead;
      __wasmfs_write = oldWrite;
    }
  }

  function testNegativeBridgeCleanup() {
    const oldMalloc = _malloc;
    const oldFree = _free;
    const oldRead = __wasmfs_read;
    const oldWrite = __wasmfs_write;
    const allocations = [];
    const frees = [];
    let readCalls = 0;
    let writeCalls = 0;
    _malloc = function(size) {
      const ptr = oldMalloc(size);
      allocations.push(ptr);
      return ptr;
    };
    _free = function(ptr) {
      frees.push(ptr);
      return oldFree(ptr);
    };
    __wasmfs_read = function() {
      ++readCalls;
      return -ebadf;
    };
    __wasmfs_write = function() {
      ++writeCalls;
      return -ebadf;
    };
    try {
      const readError =
        getError(() => FS.read({fd: 0}, new Uint8Array(1), 0, 1));
      const writeError =
        getError(() => FS.write({fd: 0}, new Uint8Array([0]), 0, 1));
      return hasErrno(readError, ebadf) && hasErrno(writeError, ebadf) &&
             readCalls === 1 && writeCalls === 1 &&
             allocations.length === 2 && allocations[0] && allocations[1] &&
             frees.length === 2 && frees[0] === allocations[0] &&
             frees[1] === allocations[1];
    } finally {
      _malloc = oldMalloc;
      _free = oldFree;
      __wasmfs_read = oldRead;
      __wasmfs_write = oldWrite;
    }
  }

  function testReadCopyCleanup() {
    const oldMalloc = _malloc;
    const oldFree = _free;
    const oldRead = __wasmfs_read;
    const allocations = [];
    const frees = [];
    let readCalls = 0;
    _malloc = function(size) {
      const ptr = oldMalloc(size);
      allocations.push(ptr);
      return ptr;
    };
    _free = function(ptr) {
      frees.push(ptr);
      return oldFree(ptr);
    };
    __wasmfs_read = function(fd, buffer, length) {
      ++readCalls;
      HEAPU8[buffer] = 0x41;
      return 1;
    };
    const expected = new Error('injected read destination failure');
    const destination = {
      set() {
        throw expected;
      },
    };
    try {
      const error = getError(() => FS.read({fd: 0}, destination, 0, 1));
      return error === expected && readCalls === 1 &&
             allocations.length === 1 && allocations[0] &&
             frees.length === 1 && frees[0] === allocations[0];
    } finally {
      _malloc = oldMalloc;
      _free = oldFree;
      __wasmfs_read = oldRead;
    }
  }

  function testWriteGetterCleanup() {
    const oldMalloc = _malloc;
    const oldFree = _free;
    const oldWrite = __wasmfs_write;
    const allocations = [];
    const frees = [];
    let writeCalls = 0;
    _malloc = function(size) {
      const ptr = oldMalloc(size);
      allocations.push(ptr);
      return ptr;
    };
    _free = function(ptr) {
      frees.push(ptr);
      return oldFree(ptr);
    };
    __wasmfs_write = function() {
      ++writeCalls;
      return 0;
    };
    const expected = new Error('injected write source failure');
    const source = {};
    Object.defineProperty(source, 0, {
      get() {
        throw expected;
      },
    });
    try {
      const error = getError(() => FS.write({fd: 0}, source, 0, 1));
      return error === expected && writeCalls === 0 &&
             allocations.length === 1 && allocations[0] &&
             frees.length === 1 && frees[0] === allocations[0];
    } finally {
      _malloc = oldMalloc;
      _free = oldFree;
      __wasmfs_write = oldWrite;
    }
  }

  if (!testInvalidArguments()) return 1;
  if (!testZeroLengthValidation()) return 2;
  if (!testMallocFailure()) return 3;
  if (!testNegativeBridgeCleanup()) return 4;
  if (!testReadCopyCleanup()) return 5;
  if (!testWriteGetterCleanup()) return 6;
  return 0;
});

int main(void) {
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  assert(exerciseJsIoMemory(EBADF, EINVAL, ENOMEM, ESPIPE,
                            pipefd[0], pipefd[1]) == 0);
  assert(close(pipefd[0]) == 0);
  assert(close(pipefd[1]) == 0);
  puts("ok");
  return 0;
}
