/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <dirent.h>
#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

enum ErrorOperation {
  Readdir,
  Open,
  Close,
};

static const char MountPath[] = "/wasmfs-node-error";
static const char FilePath[] =
  "/wasmfs-node-error/wasmfs-node-error-fixture";

// This runs in the application runtime context. In the PROXY_TO_PTHREAD
// configuration that is the Node worker which executes the backend calls, not
// the launcher thread.
EM_JS(void, injectUnknownNodeError, (int operation), {
  const state = {
    closeSync: fs.closeSync,
    openSync: fs.openSync,
    readdirSync: fs.readdirSync,
  };
  globalThis.wasmfsNodeErrorTest = state;

  const unknownError = () => {
    const error = new Error('injected unknown Node error');
    error.code = 'E_WASMFS_TEST_UNKNOWN';
    return error;
  };

  if (operation == 0) {
    fs.readdirSync = function(path, ...args) {
      if (path == '.') {
        throw unknownError();
      }
      return state.readdirSync.call(this, path, ...args);
    };
  } else if (operation == 1) {
    fs.openSync = function(path, ...args) {
      if (path == './wasmfs-node-error-fixture') {
        throw unknownError();
      }
      return state.openSync.call(this, path, ...args);
    };
  } else {
    fs.closeSync = function(fd, ...args) {
      state.failedCloseFD = fd;
      throw unknownError();
    };
  }
});

EM_JS(int, restoreUnknownNodeError, (int closeFailedFD), {
  const state = globalThis.wasmfsNodeErrorTest;
  if (!state) {
    return 1;
  }
  fs.closeSync = state.closeSync;
  fs.openSync = state.openSync;
  fs.readdirSync = state.readdirSync;
  delete globalThis.wasmfsNodeErrorTest;

  if (closeFailedFD) {
    if (state.failedCloseFD === undefined) {
      return 1;
    }
    state.closeSync.call(fs, state.failedCloseFD);
  }
  return 0;
});

EM_JS(int, expectFSReaddirError, (const char* path, int expectedErrno), {
  var error;
  try {
    FS.readdir(UTF8ToString(path));
  } catch (err) {
    error = err;
  }
  return error && error.name === 'ErrnoError' &&
                 error.errno === expectedErrno ? 0 : 1;
});

static void testReaddirError(void) {
  injectUnknownNodeError(Readdir);
  assert(expectFSReaddirError(MountPath, EIO) == 0);
  errno = 0;
  assert(opendir(MountPath) == NULL);
  assert(errno == EIO);
  assert(restoreUnknownNodeError(0) == 0);
}

static void testOpenError(void) {
  injectUnknownNodeError(Open);
  errno = 0;
  assert(open(FilePath, O_RDONLY) == -1);
  assert(errno == EIO);
  assert(restoreUnknownNodeError(0) == 0);
}

static void testCloseError(void) {
  int fd = open(FilePath, O_RDONLY);
  assert(fd >= 0);

  injectUnknownNodeError(Close);
  errno = 0;
  assert(close(fd) == -1);
  assert(errno == EIO);
  // The failed close removes the WasmFS descriptor even though the host fd is
  // still live. Close that captured host fd after restoring the Node API.
  assert(restoreUnknownNodeError(1) == 0);
}

int main(void) {
  backend_t backend = wasmfs_create_node_backend(".");
  assert(backend);
  assert(wasmfs_create_directory(MountPath, 0700, backend) == 0);

  int fd = wasmfs_create_file(FilePath, 0600, backend);
  assert(fd >= 0);
  assert(close(fd) == 0);

  testReaddirError();
  testOpenError();
  testCloseError();

  assert(unlink(FilePath) == 0);
  assert(wasmfs_unmount(MountPath) == 0);
  puts("ok");
  return 0;
}
