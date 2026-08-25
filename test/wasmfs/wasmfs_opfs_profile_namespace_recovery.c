// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This test is deliberately narrower than a power-loss, browser-crash,
// renderer-crash, SQLite, LevelDB, or Chromium-profile recovery test. It
// disposes an iframe after a test-only hook has stopped one logical namespace
// commit at a documented selector-publication boundary. A fresh leased module
// then checks only the two complete namespace states produced by that one
// populated-directory rename. The host page never reads or writes OPFS.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER)
#error "select an OPFS profile namespace recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER) != 1
#error "select exactly one OPFS profile namespace recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
#define WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT 0
#endif

#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT != 0 && \
  WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT != 1 && \
  WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT != 2
#error "WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT must be 0, 1, or 2"
#endif

// This declaration intentionally remains local to the focused test until the
// opt-in backend lands in the public WasmFS header. Keeping it here lets the
// test's C source be syntax-checked before that implementation is merged.
extern backend_t wasmfs_create_opfs_profile_namespace_backend(
  const char* profile_name);

enum TestRole {
  kOwner,
  kMutator,
  kVerifier,
};

enum TestResult {
  kReady,
  kBusy,
  kStagedTree,
  kPublishedTree,
  kFailure,
};

enum SelectorPublicationPhase {
  // The payload has already been flushed, but no alternate selector has been
  // made durable. A fresh namespace instance must choose the old root.
  kBeforeSelectorPublication = 1,
  // The alternate selector has been flushed, but the mutator has not returned
  // to its caller. A fresh namespace instance must choose the new root.
  kAfterSelectorPublication = 2,
};

static const char kProfileName[] = "wasmfs-opfs-profile-namespace-recovery";
static const char kMountPath[] = "/profile";
static const char kStagedPath[] = "/profile/staged";
static const char kStagedCachePath[] = "/profile/staged/Cache";
static const char kStagedIndexPath[] = "/profile/staged/Cache/Index";
static const char kStagedManifestPath[] =
  "/profile/staged/Cache/Index/manifest";
static const char kStagedBlobPath[] = "/profile/staged/Cache/Index/blob";
static const char kPublishedPath[] = "/profile/Live";
#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER) || \
  defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER)
static const char kPublishedCachePath[] = "/profile/Live/Cache";
static const char kPublishedIndexPath[] = "/profile/Live/Cache/Index";
static const char kPublishedManifestPath[] =
  "/profile/Live/Cache/Index/manifest";
static const char kPublishedBlobPath[] = "/profile/Live/Cache/Index/blob";
#endif
static const char kManifestContents[] = "profile-namespace-generation-A";
static const char kBlobContents[] = "nested-index-payload-A";

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
        type: 'wasmfs-opfs-profile-namespace-recovery',
      },
      window.location.origin);
  }, role, result, error);
}

static void ReportResult(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportResultOnBrowserThread, role, result, error);
}

#if WASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT
static void ReportSelectorInterruptionOnBrowserThread(int phase) {
  EM_ASM({
    const channel = new BroadcastChannel(
      'wasmfs-opfs-profile-namespace-recovery');
    channel.postMessage({
      phase: $0,
      type: 'wasmfs-opfs-profile-namespace-recovery',
    });
    channel.close();
  }, phase);
}

// This private test bridge is linked only into the mutator binary. The
// namespace backend calls it only in the matching libwasmfs test variation:
// phase 1 immediately before its alternate selector write, and phase 2
// immediately after that selector's successful flush. It deliberately never
// returns, so iframe disposal is the sole release path being exercised.
void wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(int phase) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VI, ReportSelectorInterruptionOnBrowserThread, phase);
  while (1) {
    emscripten_thread_sleep(1000);
  }
}
#endif

static int MountNamespace(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_namespace_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }

  int result = wasmfs_create_directory(kMountPath, 0700, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainNamespace(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  if (result != 0 || details.error != 0 || !details.backend_sealed ||
      !details.lease_released || !details.backend_retired) {
    return EIO;
  }
  return 0;
}

static int FinishNamespace(backend_t backend, int error) {
  int drain_error = DrainNamespace(backend);
  return error ? error : drain_error;
}

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER) || \
  defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER)
static int RemoveFileIfPresent(const char* path) {
  errno = 0;
  if (unlink(path) == 0 || errno == ENOENT) {
    return 0;
  }
  return ErrorOrEIO();
}

static int RemoveDirectoryIfPresent(const char* path) {
  errno = 0;
  if (rmdir(path) == 0 || errno == ENOENT) {
    return 0;
  }
  return ErrorOrEIO();
}

static int RemoveKnownTree(const char* root,
                           const char* cache,
                           const char* index,
                           const char* manifest,
                           const char* blob) {
  int error = RemoveFileIfPresent(manifest);
  if (error == 0) {
    error = RemoveFileIfPresent(blob);
  }
  if (error == 0) {
    error = RemoveDirectoryIfPresent(index);
  }
  if (error == 0) {
    error = RemoveDirectoryIfPresent(cache);
  }
  if (error == 0) {
    error = RemoveDirectoryIfPresent(root);
  }
  return error;
}

static int FlushDirectory(const char* path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = 0;
  if (fsync(fd) != 0) {
    error = ErrorOrEIO();
  }
  if (close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  return error;
}
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER)
static int WriteDurableFile(const char* path, const char* contents) {
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
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
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR) || \
  defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER)
static int VerifyExactFile(const char* path, const char* expected) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }

  char actual[128] = {};
  size_t size = strlen(expected);
  int error = 0;
  if (size >= sizeof(actual) ||
      read(fd, actual, sizeof(actual)) != (ssize_t)size ||
      memcmp(actual, expected, size) != 0) {
    error = EIO;
  }
  if (close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  return error;
}

static int VerifyDirectory(const char* path) {
  struct stat stat_buffer;
  if (stat(path, &stat_buffer) != 0) {
    return ErrorOrEIO();
  }
  return S_ISDIR(stat_buffer.st_mode) ? 0 : EIO;
}

static int VerifyTree(const char* root,
                      const char* cache,
                      const char* index,
                      const char* manifest,
                      const char* blob) {
  int error = VerifyDirectory(root);
  if (error == 0) {
    error = VerifyDirectory(cache);
  }
  if (error == 0) {
    error = VerifyDirectory(index);
  }
  if (error == 0) {
    error = VerifyExactFile(manifest, kManifestContents);
  }
  if (error == 0) {
    error = VerifyExactFile(blob, kBlobContents);
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
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER)
static int PrepareFixture(void) {
  int error = RemoveKnownTree(kStagedPath,
                              kStagedCachePath,
                              kStagedIndexPath,
                              kStagedManifestPath,
                              kStagedBlobPath);
  if (error == 0) {
    error = RemoveKnownTree(kPublishedPath,
                            kPublishedCachePath,
                            kPublishedIndexPath,
                            kPublishedManifestPath,
                            kPublishedBlobPath);
  }
  if (error == 0 && mkdir(kStagedPath, 0700) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0 && mkdir(kStagedCachePath, 0700) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0 && mkdir(kStagedIndexPath, 0700) != 0) {
    error = ErrorOrEIO();
  }
  if (error == 0) {
    error = WriteDurableFile(kStagedManifestPath, kManifestContents);
  }
  if (error == 0) {
    error = WriteDurableFile(kStagedBlobPath, kBlobContents);
  }
  // The profile-namespace backend must turn these into container commits. A
  // direct OPFS directory intentionally returns ENOTSUP instead.
  if (error == 0) {
    error = FlushDirectory(kStagedIndexPath);
  }
  if (error == 0) {
    error = FlushDirectory(kStagedCachePath);
  }
  if (error == 0) {
    error = FlushDirectory(kStagedPath);
  }
  if (error == 0) {
    error = FlushDirectory(kMountPath);
  }
  return error;
}
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER)
static int VerifyDisposition(void) {
  if (VerifyTree(kStagedPath,
                 kStagedCachePath,
                 kStagedIndexPath,
                 kStagedManifestPath,
                 kStagedBlobPath) == 0 &&
      IsMissing(kPublishedPath) == 0) {
    return kStagedTree;
  }
  if (VerifyTree(kPublishedPath,
                 kPublishedCachePath,
                 kPublishedIndexPath,
                 kPublishedManifestPath,
                 kPublishedBlobPath) == 0 &&
      IsMissing(kStagedPath) == 0) {
    return kPublishedTree;
  }
  return kFailure;
}
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER)
static int CleanupFixture(void) {
  int error = RemoveKnownTree(kStagedPath,
                              kStagedCachePath,
                              kStagedIndexPath,
                              kStagedManifestPath,
                              kStagedBlobPath);
  if (error == 0) {
    error = RemoveKnownTree(kPublishedPath,
                            kPublishedCachePath,
                            kPublishedIndexPath,
                            kPublishedManifestPath,
                            kPublishedBlobPath);
  }
  if (error == 0) {
    error = FlushDirectory(kMountPath);
  }
  return error;
}
#endif

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = MountNamespace(&backend);

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER)
  const int role = kOwner;
#elif defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR)
  const int role = kMutator;
#else
  const int role = kVerifier;
#endif

  if (error != 0) {
    if (backend) {
      error = FinishNamespace(backend, error);
    }
    ReportResult(role, error == EBUSY ? kBusy : kFailure, error);
  } else {
#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER)
    error = PrepareFixture();
    error = FinishNamespace(backend, error);
    ReportResult(kOwner, error == 0 ? kReady : kFailure, error);
#elif defined(WASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR)
    error = VerifyTree(kStagedPath,
                       kStagedCachePath,
                       kStagedIndexPath,
                       kStagedManifestPath,
                       kStagedBlobPath);
    if (error == 0) {
      error = IsMissing(kPublishedPath);
    }
    if (error != 0) {
      error = FinishNamespace(backend, error);
      ReportResult(kMutator, kFailure, error);
    } else {
      // The controller waits for the backend's selector hook before it
      // disposes this iframe. Do not treat that disposal as orderly shutdown.
      ReportResult(kMutator, kReady, 0);
      if (rename(kStagedPath, kPublishedPath) == 0) {
        // A matching test hook must have stopped the call. Returning here
        // would make the harness time out rather than pass silently.
        error = EIO;
      } else {
        error = ErrorOrEIO();
      }
      error = FinishNamespace(backend, error);
      ReportResult(kMutator, kFailure, error);
    }
#else
    int disposition = VerifyDisposition();
    if (disposition == kFailure) {
      error = EIO;
    }
    if (error == 0) {
      error = CleanupFixture();
    }
    error = FinishNamespace(backend, error);
    ReportResult(kVerifier,
                 error == 0 ? disposition : kFailure,
                 error);
#endif
  }

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
