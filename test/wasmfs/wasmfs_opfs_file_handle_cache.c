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

enum TestPhase {
  kPrepared,
  kIdle,
  kFinished,
};

enum TestCommand {
  kNoCommand,
  kStart,
  kContinue,
  kShutdown,
};

static const char kMountPath[] = "/opfs";
static const char kAliasMountPath[] = "/opfs-alias";
static char old_path[96];
static char new_path[96];
static char probe_path[96];
static char probe_alias_path[96];

static _Atomic int command;
static backend_t backend;
static int phase = kPrepared;
static int test_error;
static ino_t initial_inode;

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportPhaseOnBrowserThread(int phase, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        phase: $0,
        type: 'wasmfs-opfs-file-handle-cache',
      },
      window.location.origin);
  }, phase, error);
}

static void ReportPhase(int phase, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportPhaseOnBrowserThread, phase, error);
}

static int MountOPFS(void) {
  backend = wasmfs_create_opfs_backend();
  if (!backend) {
    return ErrorOrEIO();
  }

  int err = wasmfs_create_directory(kMountPath, 0777, backend);
  if (err != 0) {
    return err < 0 ? -err : EIO;
  }
  return 0;
}

static int FormatPath(char* path,
                      size_t path_size,
                      const char* mount,
                      unsigned long long nonce,
                      const char* suffix) {
  int length = snprintf(path, path_size,
                        "%s/wasmfs-opfs-file-handle-cache-%llu-%s", mount,
                        nonce, suffix);
  if (length < 0 || length >= (int)path_size) {
    return ENAMETOOLONG;
  }
  return 0;
}

static int Prepare(void) {
  int error = MountOPFS();
  if (error != 0) {
    return error;
  }

  // Use a fresh name on every invocation so that the trace starts with no
  // FileSystemFileHandle activity from cleanup of a prior interrupted run.
  unsigned long long nonce = (unsigned long long)emscripten_get_now();
  if ((error = FormatPath(old_path, sizeof(old_path), kMountPath, nonce,
                          "old")) != 0 ||
      (error = FormatPath(new_path, sizeof(new_path), kMountPath, nonce,
                          "new")) != 0 ||
      (error = FormatPath(probe_path, sizeof(probe_path), kMountPath, nonce,
                          "probe")) != 0 ||
      (error = FormatPath(probe_alias_path, sizeof(probe_alias_path),
                          kAliasMountPath, nonce, "probe")) != 0) {
    return error;
  }
  return 0;
}

static int ExerciseMetadataLookup(void) {
  int fd = open(probe_path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  if (close(fd) != 0) {
    return ErrorOrEIO();
  }

  int error = wasmfs_create_directory(kAliasMountPath, 0777, backend);
  if (error != 0) {
    return error < 0 ? -error : EIO;
  }
  // The alias mount has an empty dcache. chmod must discover a fresh
  // OPFSFile wrapper without retaining a FileSystemFileHandle slot.
  if (chmod(probe_alias_path, 0600) != 0) {
    return ErrorOrEIO();
  }
  if (wasmfs_unmount(kAliasMountPath) != 0) {
    return ErrorOrEIO();
  }
  if (unlink(probe_path) != 0) {
    return ErrorOrEIO();
  }
  return 0;
}

static int CreateAndClose(void) {
  int fd = open(old_path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }

  char marker = 'A';
  if (pwrite(fd, &marker, 1, 0) != 1 || fdatasync(fd) != 0) {
    int error = ErrorOrEIO();
    (void)close(fd);
    return error;
  }

  struct stat stat_buf;
  if (fstat(fd, &stat_buf) != 0) {
    int error = ErrorOrEIO();
    (void)close(fd);
    return error;
  }
  initial_inode = stat_buf.st_ino;

  if (close(fd) != 0) {
    return ErrorOrEIO();
  }
  return 0;
}

static int ReacquireRenameAndReopen(void) {
  struct stat stat_buf;
  if (stat(old_path, &stat_buf) != 0) {
    return ErrorOrEIO();
  }
  if (stat_buf.st_ino != initial_inode) {
    return EIO;
  }

  // Path truncate is another one-shot file-handle operation. It must not
  // leave an idle JS handle before the following rename.
  if (truncate(old_path, 1) != 0) {
    return ErrorOrEIO();
  }

  // rename must reacquire the idle old-path handle and update the retained
  // wrapper's locator before its later open under the new name.
  if (rename(old_path, new_path) != 0) {
    return ErrorOrEIO();
  }

  errno = 0;
  int old_fd = open(old_path, O_RDONLY);
  if (old_fd >= 0) {
    (void)close(old_fd);
    return EIO;
  }
  if (errno != ENOENT) {
    return ErrorOrEIO();
  }

  int fd = open(new_path, O_RDWR);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  if (fstat(fd, &stat_buf) != 0) {
    int error = ErrorOrEIO();
    (void)close(fd);
    return error;
  }
  if (stat_buf.st_ino != initial_inode) {
    (void)close(fd);
    return EIO;
  }

  char marker = 0;
  if (pread(fd, &marker, 1, 0) != 1 || marker != 'A') {
    (void)close(fd);
    return EIO;
  }
  marker = 'B';
  if (pwrite(fd, &marker, 1, 0) != 1 || fdatasync(fd) != 0) {
    int error = ErrorOrEIO();
    (void)close(fd);
    return error;
  }
  if (close(fd) != 0) {
    return ErrorOrEIO();
  }

  // A path stat after the final close must lazily reacquire and release the
  // file handle again without changing the dcache wrapper's inode.
  if (stat(new_path, &stat_buf) != 0) {
    return ErrorOrEIO();
  }
  if (stat_buf.st_ino != initial_inode || stat_buf.st_size != 1) {
    return EIO;
  }

  if (unlink(new_path) != 0) {
    return ErrorOrEIO();
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_file_handle_cache_start(void) {
  atomic_store(&command, kStart);
}

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_file_handle_cache_continue(void) {
  atomic_store(&command, kContinue);
}

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_file_handle_cache_shutdown(void) {
  atomic_store(&command, kShutdown);
}

static void MainLoop(void) {
  switch (atomic_exchange(&command, kNoCommand)) {
    case kNoCommand:
      return;
    case kStart:
      if (phase != kPrepared) {
        return;
      }
      test_error = CreateAndClose();
      phase = kIdle;
      ReportPhase(phase, test_error);
      return;
    case kContinue:
      if (phase != kIdle || test_error != 0) {
        return;
      }
      test_error = ReacquireRenameAndReopen();
      if (test_error == 0) {
        test_error = ExerciseMetadataLookup();
      }
      phase = kFinished;
      ReportPhase(phase, test_error);
      return;
    case kShutdown:
      exit(test_error == 0 ? 0 : 1);
  }
}

int main(void) {
  test_error = Prepare();
  ReportPhase(kPrepared, test_error);
  if (test_error != 0) {
    return 1;
  }
  emscripten_set_main_loop(MainLoop, 0, false);
}
