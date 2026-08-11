// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

enum TestRole {
  kHolder,
  kContender,
};

static const char kVictimPath[] = "/opfs/wasmfs-dup3-close-victim";

#ifdef WASMFS_DUP3_CLOSE_HOLDER
static const char kSourcePath[] = "/opfs/wasmfs-dup3-close-source";
static _Atomic int shutdown_requested;
static int source_fd = -1;
static int replacement_fd = -1;
#endif

static void ReportResultOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-dup3-close',
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
    return errno ? errno : EIO;
  }

  int err = wasmfs_create_directory("/opfs", 0777, backend);
  if (err != 0) {
    return err < 0 ? -err : EIO;
  }
  return 0;
}

#ifdef WASMFS_DUP3_CLOSE_HOLDER

EMSCRIPTEN_KEEPALIVE
void wasmfs_dup3_close_holder_shutdown(void) {
  atomic_store(&shutdown_requested, 1);
}

static int CloseHolderDescriptors(void) {
  int error = 0;
  if (replacement_fd >= 0) {
    int fd = replacement_fd;
    replacement_fd = -1;
    if (close(fd) != 0) {
      error = errno ? errno : EIO;
    }
  }
  if (source_fd >= 0) {
    int fd = source_fd;
    source_fd = -1;
    if (close(fd) != 0 && error == 0) {
      error = errno ? errno : EIO;
    }
  }
  return error;
}

static void ShutdownWhenRequested(void) {
  if (atomic_exchange(&shutdown_requested, 0)) {
    // Both descriptors refer to the replacement OpenFileState. Explicitly
    // close them before global WasmFS teardown so the test also observes a
    // clean module exit rather than relying on iframe destruction.
    exit(CloseHolderDescriptors() == 0 ? 0 : 1);
  }
}

int main(void) {
  int error = MountOPFS();
  if (error == 0) {
    // The test may be rerun against the same OPFS origin.
    (void)unlink(kVictimPath);
    (void)unlink(kSourcePath);

    int victim = open(kVictimPath, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (victim < 0) {
      error = errno;
    } else {
      int source = open(kSourcePath, O_CREAT | O_EXCL | O_RDWR, 0600);
      if (source < 0) {
        error = errno;
        (void)close(victim);
      } else if (dup3(source, victim, 0) < 0) {
        error = errno;
        (void)close(source);
        (void)close(victim);
      } else {
        // Keep both aliases open until the parent has proven that the
        // overwritten victim's access handle was released. The shutdown hook
        // closes these explicitly before exit.
        source_fd = source;
        replacement_fd = victim;
      }
    }
  }

  ReportResult(kHolder, error);
  emscripten_set_main_loop(ShutdownWhenRequested, 0, false);
}

#else

static void KeepRuntimeAlive(void) {}

int main(void) {
  int error = MountOPFS();
  if (error == 0) {
    // This open requires a new SyncAccessHandle for the original victim file.
    // It can succeed while the holder module is still alive only if dup3
    // closed the holder's overwritten final descriptor.
    int victim = open(kVictimPath, O_RDWR);
    if (victim < 0) {
      error = errno;
    } else {
      static const char marker = 'x';
      if (pwrite(victim, &marker, 1, 0) != 1) {
        error = errno ? errno : EIO;
      } else if (fdatasync(victim) != 0) {
        error = errno;
      } else if (close(victim) != 0) {
        error = errno;
      }
    }
  }

  ReportResult(kContender, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}

#endif
