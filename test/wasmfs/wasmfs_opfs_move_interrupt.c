// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_MOVE_INTERRUPT_OWNER) && \
  !defined(WASMFS_OPFS_MOVE_INTERRUPT_MUTATOR) && \
  !defined(WASMFS_OPFS_MOVE_INTERRUPT_VERIFIER)
#error "select an OPFS move interruption test role"
#endif

enum TestRole {
  kOwner,
  kMutator,
  kVerifier,
};

enum TestResult {
  kReady,
  kBusy,
  kPreMove,
  kPostMove,
  kFailure,
};

static const char kProfileName[] = "wasmfs-opfs-move-interrupt";
static const char kCommitPath[] = "/opfs/wasmfs-opfs-move-interrupt-commit";
static const char kTemporaryPath[] =
  "/opfs/wasmfs-opfs-move-interrupt-commit.tmp";
static const char kGenerationA[] = "A";
static const char kGenerationB[] = "B";

static int ErrorOrEIO(void) {
  return errno ? errno : EIO;
}

static void ReportResultOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-move-interrupt',
      },
      window.location.origin);
  }, role, result, error);
}

static void ReportResult(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportResultOnBrowserThread, role, result, error);
}

static int MountOPFS(void) {
  errno = 0;
  backend_t backend =
    wasmfs_create_opfs_backend_with_profile_lease(kProfileName);
  if (!backend) {
    return ErrorOrEIO();
  }

  int err = wasmfs_create_directory("/opfs", 0777, backend);
  if (err != 0) {
    return err < 0 ? -err : EIO;
  }
  return 0;
}

static int RemoveIfPresent(const char* path) {
  errno = 0;
  if (unlink(path) == 0 || errno == ENOENT) {
    return 0;
  }
  return ErrorOrEIO();
}

static int WriteDurableFile(const char* path, const char* contents) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }

  size_t size = strlen(contents);
  int error = 0;
  if (pwrite(fd, contents, size, 0) != (ssize_t)size) {
    error = ErrorOrEIO();
  } else if (fdatasync(fd) != 0) {
    error = ErrorOrEIO();
  }
  if (close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  return error;
}

static int VerifyExactFile(const char* path, const char* expected) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }

  char actual[2] = {};
  size_t size = strlen(expected);
  int error = 0;
  ssize_t bytes_read = read(fd, actual, sizeof(actual));
  if (size >= sizeof(actual) || bytes_read != (ssize_t)size ||
      memcmp(actual, expected, size) != 0) {
    error = EIO;
  }
  if (close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  return error;
}

static int IsMissing(const char* path) {
  errno = 0;
  if (access(path, F_OK) == 0) {
    return EEXIST;
  }
  return errno == ENOENT ? 0 : ErrorOrEIO();
}

static int PrepareFixture(void) {
  int error = RemoveIfPresent(kTemporaryPath);
  if (error == 0) {
    error = RemoveIfPresent(kCommitPath);
  }
  if (error == 0) {
    error = WriteDurableFile(kCommitPath, kGenerationA);
  }
  return error;
}

static int CleanupFixture(void) {
  int error = RemoveIfPresent(kTemporaryPath);
  if (error == 0) {
    error = RemoveIfPresent(kCommitPath);
  }
  return error;
}

static int VerifyDisposition(void) {
  // The verifier runs in a fresh leased module after iframe disposal. It
  // observes only the two known namespace/data dispositions used by this
  // controlled test: A plus tmp-B, or B with no temporary file.
  int error = VerifyExactFile(kCommitPath, kGenerationA);
  if (error == 0 && VerifyExactFile(kTemporaryPath, kGenerationB) == 0) {
    return kPreMove;
  }

  error = VerifyExactFile(kCommitPath, kGenerationB);
  if (error == 0 && IsMissing(kTemporaryPath) == 0) {
    return kPostMove;
  }
  return kFailure;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  int error = MountOPFS();
#if defined(WASMFS_OPFS_MOVE_INTERRUPT_OWNER)
  int role = kOwner;
#elif defined(WASMFS_OPFS_MOVE_INTERRUPT_MUTATOR)
  int role = kMutator;
#else
  int role = kVerifier;
#endif

  if (error != 0) {
    ReportResult(role, error == EBUSY ? kBusy : kFailure, error);
  } else {
#if defined(WASMFS_OPFS_MOVE_INTERRUPT_OWNER)
    error = PrepareFixture();
    ReportResult(kOwner, error == 0 ? kReady : kFailure, error);
#elif defined(WASMFS_OPFS_MOVE_INTERRUPT_MUTATOR)
    error = WriteDurableFile(kTemporaryPath, kGenerationB);
    ReportResult(kMutator, error == 0 ? kReady : kFailure, error);
    if (error == 0 && rename(kTemporaryPath, kCommitPath) != 0) {
      ReportResult(kMutator, kFailure, ErrorOrEIO());
    }
#else
    int result = VerifyDisposition();
    // Cleanup follows the fresh-instance observation so it cannot determine
    // the reported disposition or turn this into a teardown test.
    int cleanup_error = CleanupFixture();
    if (result == kFailure || cleanup_error != 0) {
      ReportResult(kVerifier, kFailure,
                   cleanup_error ? cleanup_error : EIO);
    } else {
      ReportResult(kVerifier, result, 0);
    }
#endif
  }

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
