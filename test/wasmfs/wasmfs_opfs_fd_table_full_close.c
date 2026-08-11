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

// WASMFS_FD_MAX is an internal WasmFS implementation limit. Keep this test
// tied to that fixed table size without exposing it as a public API contract.
static const int kLastWasmfsDescriptor = 4095;
static const char kVictimPath[] = "/opfs/wasmfs-fd-table-full-victim";

#ifdef WASMFS_FD_TABLE_FULL_HOLDER
static _Atomic int shutdown_requested;
#endif

static void ReportResultOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-fd-table-full-close',
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

#ifdef WASMFS_FD_TABLE_FULL_HOLDER

EMSCRIPTEN_KEEPALIVE
void wasmfs_fd_table_full_holder_shutdown(void) {
  atomic_store(&shutdown_requested, 1);
}

static void ShutdownWhenRequested(void) {
  if (atomic_exchange(&shutdown_requested, 0)) {
    exit(0);
  }
}

static int FillTableAndCloseVictim(void) {
  int victim = open(kVictimPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (victim < 0) {
    return errno ? errno : EIO;
  }
  if (victim < 3 || victim > kLastWasmfsDescriptor) {
    (void)close(victim);
    return EIO;
  }

  int error = 0;
  int last_alias = 2;
  for (int fd = 3; fd <= kLastWasmfsDescriptor; ++fd) {
    if (fd == victim) {
      continue;
    }
    if (dup3(victim, fd, 0) != fd) {
      error = errno ? errno : EIO;
      break;
    }
    last_alias = fd;
  }

  if (error == 0) {
    errno = 0;
    int rejected = open(kVictimPath, O_RDWR);
    if (rejected >= 0) {
      error = EIO;
      (void)close(rejected);
    } else if (errno != EMFILE) {
      error = errno ? errno : EIO;
    }
  }

  for (int fd = 3; fd <= last_alias; ++fd) {
    if (fd == victim) {
      continue;
    }
    if (close(fd) != 0 && error == 0) {
      error = errno ? errno : EIO;
    }
  }
  if (close(victim) != 0 && error == 0) {
    error = errno ? errno : EIO;
  }
  return error;
}

int main(void) {
  int error = MountOPFS();
  if (error == 0) {
    // The test may be rerun against the same OPFS origin.
    (void)unlink(kVictimPath);
    error = FillTableAndCloseVictim();
  }

  ReportResult(kHolder, error);
  emscripten_set_main_loop(ShutdownWhenRequested, 0, false);
}

#else

static void KeepRuntimeAlive(void) {}

int main(void) {
  int error = MountOPFS();
  if (error == 0) {
    // The holder is still alive. This writable open succeeds only if its
    // rejected table-full open was closed before its real descriptor closed.
    int victim = open(kVictimPath, O_RDWR);
    if (victim < 0) {
      error = errno ? errno : EIO;
    } else {
      static const char marker = 'f';
      if (pwrite(victim, &marker, 1, 0) != 1) {
        error = errno ? errno : EIO;
      } else if (fdatasync(victim) != 0) {
        error = errno ? errno : EIO;
      } else if (close(victim) != 0) {
        error = errno ? errno : EIO;
      }
    }
  }

  ReportResult(kContender, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}

#endif
