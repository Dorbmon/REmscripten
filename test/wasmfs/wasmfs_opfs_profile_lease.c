// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

enum LeaseResult {
  kLeaseAcquired,
  kLeaseBusy,
  kLeaseOtherFailure,
};

static _Atomic int shutdown_requested;

static int WriteProfileMarker(void) {
  static const char marker[] = "wasmfs profile lease";
  int fd = open("/opfs/profile-lease-data", O_CREAT | O_RDWR, 0600);
  if (fd < 0) {
    return errno;
  }
  if (pwrite(fd, marker, sizeof(marker), 0) != sizeof(marker)) {
    int err = errno;
    if (err == 0) {
      err = EIO;
    }
    (void)close(fd);
    return err;
  }
  if (fdatasync(fd) != 0) {
    int err = errno;
    (void)close(fd);
    return err;
  }
  if (close(fd) != 0) {
    return errno;
  }
  return 0;
}

static void ReportLeaseResultOnBrowserThread(int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        result: $0,
        type: 'wasmfs-opfs-profile-lease',
      },
      window.location.origin);
  }, result, error);
}

EMSCRIPTEN_KEEPALIVE
void wasmfs_test_request_profile_lease_shutdown(void) {
  atomic_store(&shutdown_requested, 1);
}

static void ShutdownWhenRequested(void) {
  if (atomic_exchange(&shutdown_requested, 0)) {
    // This callback runs on the application pthread. `exit` therefore runs
    // WasmFS global teardown before Emscripten terminates the module runtime.
    exit(0);
  }
}

int main(void) {
  errno = 0;
  backend_t backend =
    wasmfs_create_opfs_backend_with_profile_lease("contention-test");
  int error = errno;
  int result = kLeaseOtherFailure;
  if (!backend) {
    // The browser test checks this status, so a contender only succeeds if the
    // factory failed immediately with the documented EBUSY result.
    result = error == EBUSY ? kLeaseBusy : kLeaseOtherFailure;
  } else if (wasmfs_create_directory("/opfs", 0777, backend) == 0) {
    error = WriteProfileMarker();
    if (error == 0) {
      result = kLeaseAcquired;
    }
  } else {
    error = errno;
  }

  // PROXY_TO_PTHREAD runs main in a worker. Report from the browser main
  // runtime so the iframe's parent can sequence independent module instances.
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportLeaseResultOnBrowserThread, result, error);
  emscripten_set_main_loop(ShutdownWhenRequested, 0, false);
}
