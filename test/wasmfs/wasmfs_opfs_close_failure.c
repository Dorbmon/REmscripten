// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

enum TestRole {
  kHolder,
  kContender,
};

static const char kTargetPath[] = "/opfs/wasmfs-opfs-close-failure-target";
static const char kMovedPath[] = "/opfs/wasmfs-opfs-close-failure-moved";
static const char kOtherPath[] = "/opfs/wasmfs-opfs-close-failure-other";

#ifdef WASMFS_OPFS_CLOSE_FAILURE_HOLDER
static _Atomic int shutdown_requested;
#endif

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportResultOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-close-failure',
      },
      window.location.origin);
  }, role, error);
}

static void ReportResult(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportResultOnBrowserThread, role, error);
}

static int MountOPFS(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  if (!backend) {
    return ErrorOrEIO();
  }

  int err = wasmfs_create_directory("/opfs", 0777, backend);
  if (err != 0) {
    return err < 0 ? -err : EIO;
  }
  return 0;
}

#ifdef WASMFS_OPFS_CLOSE_FAILURE_HOLDER

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_close_failure_holder_shutdown(void) {
  atomic_store(&shutdown_requested, 1);
}

static void ShutdownWhenRequested(void) {
  if (atomic_exchange(&shutdown_requested, 0)) {
    // The target's failed close already removed its descriptor. All normally
    // closed descriptors must be gone before this teardown witness.
    exit(0);
  }
}

static int ExerciseFailedClose(void) {
  int target = open(kTargetPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (target < 0) {
    return ErrorOrEIO();
  }

  // The link-time hook throws before the browser close call. WasmFS must
  // report that failure, retain the JS access slot, and poison this wrapper.
  errno = 0;
  if (close(target) == 0) {
    return EIO;
  }
  if (errno != EIO) {
    return ErrorOrEIO();
  }

  // These operations use the dcache's existing OPFSFile wrapper. They must
  // fail closed without opening another access handle or touching the file.
  errno = 0;
  int reopened = open(kTargetPath, O_RDWR);
  if (reopened >= 0) {
    (void)close(reopened);
    return EIO;
  }
  if (errno != EIO) {
    return ErrorOrEIO();
  }

  struct stat stat_buf;
  errno = 0;
  if (stat(kTargetPath, &stat_buf) == 0) {
    return EIO;
  }
  if (errno != EIO) {
    return ErrorOrEIO();
  }

  errno = 0;
  if (truncate(kTargetPath, 0) == 0) {
    return EIO;
  }
  if (errno != EIO) {
    return ErrorOrEIO();
  }

  // Rename must reject the same poisoned wrapper rather than operating with
  // its deliberately pinned FileSystemFileHandle.
  errno = 0;
  if (rename(kTargetPath, kMovedPath) == 0) {
    return EIO;
  }
  if (errno != EIO) {
    return ErrorOrEIO();
  }

  // This unrelated file forces a subsequent access allocation. The browser
  // harness compares its trace ID with the retained target ID.
  int other = open(kOtherPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (other < 0) {
    return ErrorOrEIO();
  }
  if (close(other) != 0) {
    return ErrorOrEIO();
  }
  return 0;
}

int main(void) {
  int error = MountOPFS();
  if (error == 0) {
    // The test may be rerun against the same OPFS origin.
    (void)unlink(kTargetPath);
    (void)unlink(kMovedPath);
    (void)unlink(kOtherPath);
    error = ExerciseFailedClose();
  }

  ReportResult(kHolder, error);
  emscripten_set_main_loop(ShutdownWhenRequested, 0, false);
}

#else

static void KeepRuntimeAlive(void) {}

int main(void) {
  int error = MountOPFS();
  if (error == 0) {
    // The holder's injected failure never invoked browser close(), so this
    // independent module must not acquire a second writable access handle.
    errno = 0;
    int target = open(kTargetPath, O_RDWR);
    if (target >= 0) {
      (void)close(target);
      error = EIO;
    } else if (errno != EACCES) {
      error = ErrorOrEIO();
    }
  }

  ReportResult(kContender, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}

#endif
