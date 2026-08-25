// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This browser-coordinated test covers only the direct leased OPFS backend's
// response to a lost ProxyWorker completion acknowledgement.  It deliberately
// does not exercise the profile-namespace backend or claim crash, power-loss,
// database, or complete profile recovery semantics.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_HOLDER) && \
  !defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_CONTENDER)
#error "select a direct OPFS proxy-completion test role"
#endif

#if defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_HOLDER) + \
      defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_CONTENDER) != 1
#error "select exactly one direct OPFS proxy-completion test role"
#endif

#ifndef WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PHASE
#error "select a direct OPFS proxy-completion phase"
#endif

#if WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PHASE < 1 || \
  WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PHASE > 5
#error "invalid direct OPFS proxy-completion phase"
#endif

#ifndef WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PROFILE_NAME
#error "select a direct OPFS proxy-completion profile name"
#endif

#ifndef WASMFS_OPFS_DIRECT_PROXY_COMPLETION_MOUNT_NAME
#error "select a direct OPFS proxy-completion mount name"
#endif

#define WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY(value) \
  WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY_IMPL(value)

enum TestRole {
  kHolder,
  kContender,
};

enum TestResult {
  kReady,
  kBusy,
  kFailure,
};

enum ProxyCompletionPhase {
  kRootInitialisation = 1,
  kInsertFile = 2,
  kInsertDirectory = 3,
  kRemoveChild = 4,
  kEnumerate = 5,
};

static const char kProfileName[] =
  WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY(
    WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PROFILE_NAME);
static const char kMountPath[] =
  "/" WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY(
         WASMFS_OPFS_DIRECT_PROXY_COMPLETION_MOUNT_NAME);
static const char kSecondMountPath[] = "/direct-opfs-proxy-completion-after";
// A direct OPFS backend exposes the origin's physical OPFS root at its virtual
// mountpoint. Keep physical child names distinct across compiled phases and
// repeated browser runs; the virtual mount name alone would not isolate them.
static const char kTargetLeaf[] =
  WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY(
    WASMFS_OPFS_DIRECT_PROXY_COMPLETION_MOUNT_NAME) "-target";
static const char kAfterLeaf[] =
  WASMFS_OPFS_DIRECT_PROXY_COMPLETION_STRINGIFY(
    WASMFS_OPFS_DIRECT_PROXY_COMPLETION_MOUNT_NAME) "-after";

#if defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_HOLDER)
static _Atomic int holderShutdownRequested;

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_direct_proxy_completion_holder_shutdown(void) {
  atomic_store(&holderShutdownRequested, 1);
}
#endif

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static int MakePath(char* path, size_t size, const char* leaf) {
  const int written = snprintf(path, size, "%s/%s", kMountPath, leaf);
  return written >= 0 && (size_t)written < size ? 0 : EIO;
}

static int CreateSeedFile(const char* path) {
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  if (write(fd, "x", 1) != 1) {
    const int error = ErrorOrEIO();
    (void)close(fd);
    return error;
  }
  if (close(fd) != 0) {
    return ErrorOrEIO();
  }
  return 0;
}

static int ExpectOpenFailure(const char* path) {
  errno = 0;
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd >= 0) {
    (void)close(fd);
    return EIO;
  }
  return errno == EIO ? 0 : EIO;
}

static int ExpectMkdirFailure(const char* path) {
  errno = 0;
  if (mkdir(path, 0700) == 0) {
    return EIO;
  }
  return errno == EIO ? 0 : EIO;
}

static int ExpectUnlinkFailure(const char* path) {
  errno = 0;
  if (unlink(path) == 0) {
    return EIO;
  }
  return errno == EIO ? 0 : EIO;
}

static int ExpectEnumerationFailure(void) {
  errno = 0;
  DIR* directory = opendir(kMountPath);
  if (!directory) {
    return errno == EIO ? 0 : EIO;
  }

  // WasmFS currently snapshots direct directory entries while opening the
  // descriptor, but keep this fixture valid if a future implementation delays
  // enumeration until the first readdir call.
  errno = 0;
  struct dirent* entry = readdir(directory);
  const int error = entry == NULL && errno == EIO ? 0 : EIO;
  (void)closedir(directory);
  return error;
}

static int RemovePossibleChild(const char* path) {
  // The phase hook deliberately makes native code unable to know whether its
  // browser-side directory operation ran. Once the failed holder's document
  // has been disposed, a fresh leased backend can discard only this fixture's
  // nonce-isolated child, regardless of whether it is a file, a directory, or
  // absent. This is test-environment cleanup, not an acknowledgement of the
  // failed holder's terminal drain.
  errno = 0;
  if (unlink(path) == 0 || errno == ENOENT) {
    return 0;
  }
  errno = 0;
  if (rmdir(path) == 0 || errno == ENOENT) {
    return 0;
  }
  return ErrorOrEIO();
}

static int MountBackend(backend_t backend) {
  const int result = wasmfs_create_directory(kMountPath, 0700, backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int ExerciseLostCompletion(backend_t backend) {
  char target[PATH_MAX];
  char after[PATH_MAX];
  if (MakePath(target, sizeof(target), kTargetLeaf) ||
      MakePath(after, sizeof(after), kAfterLeaf)) {
    return EIO;
  }

  if (WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PHASE == kRootInitialisation) {
    // Root initialisation itself was not acknowledged. A later attempt to use
    // this backend must be rejected rather than proxying a second root request.
    if (wasmfs_create_directory(kMountPath, 0700, backend) != -EIO ||
        wasmfs_create_directory(kSecondMountPath, 0700, backend) != -EIO) {
      return EIO;
    }
    return 0;
  }

  if (MountBackend(backend)) {
    return EIO;
  }

  int error = 0;
  switch (WASMFS_OPFS_DIRECT_PROXY_COMPLETION_PHASE) {
    case kInsertFile:
      error = ExpectOpenFailure(target);
      break;
    case kInsertDirectory:
      error = ExpectMkdirFailure(target);
      break;
    case kRemoveChild:
      error = CreateSeedFile(target);
      if (!error) {
        error = ExpectUnlinkFailure(target);
      }
      break;
    case kEnumerate:
      error = CreateSeedFile(target);
      if (!error) {
        error = ExpectEnumerationFailure();
      }
      break;
    default:
      return EIO;
  }
  if (error) {
    return error;
  }

  // All of the direct directory paths share one browser worker. Once its
  // completion acknowledgement is lost, no later path may assume whether its
  // previous browser operation ran. A new child prevents a dcache hit from
  // hiding an accidental later proxy request.
  return ExpectOpenFailure(after);
}

static int RunHolder(void) {
  errno = 0;
  backend_t backend = wasmfs_create_opfs_backend_with_profile_lease(kProfileName);
  if (!backend) {
    return ErrorOrEIO();
  }
  const int error = ExerciseLostCompletion(backend);
  if (error) {
    return error;
  }

  // The ambiguous direct operation poisons the only safe lease handoff. The
  // global drain must report the original I/O failure and keep that live lease;
  // the parent page verifies the latter with a second module before this
  // holder document is disposed.
  wasmfs_terminal_drain_result details = {0};
  const int result = wasmfs_terminal_drain(&details);
  return result == -EIO && details.error == -EIO &&
         details.backend_terminal_failures == 1
           ? 0
           : EIO;
}

static int RunContender(void) {
  errno = 0;
  backend_t backend = wasmfs_create_opfs_backend_with_profile_lease(kProfileName);
  if (!backend) {
    return errno == EBUSY ? EBUSY : ErrorOrEIO();
  }

  char target[PATH_MAX];
  char after[PATH_MAX];
  if (MakePath(target, sizeof(target), kTargetLeaf) ||
      MakePath(after, sizeof(after), kAfterLeaf) || MountBackend(backend) ||
      RemovePossibleChild(target) || RemovePossibleChild(after)) {
    return EIO;
  }

  // A caller observes kReady only after it has both cleared the fixture's
  // possible physical side effect and completed a result-bearing terminal
  // drain. While the failed holder is still alive the parent requires kBusy;
  // after disposing that holder it uses kReady only to finish test cleanup
  // before it starts another independent browser test.
  wasmfs_terminal_drain_result details = {0};
  const int result = wasmfs_terminal_drain(&details);
  return result == 0 && details.error == 0 &&
         details.backend_terminal_failures == 0
           ? 0
           : EIO;
}

static void ReportResultOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-direct-proxy-completion',
      },
      window.location.origin);
  }, role, result, error);
}

static void ReportResult(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportResultOnBrowserThread, role, result, error);
}

static void KeepRuntimeAlive(void) {
#if defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_HOLDER)
  if (atomic_exchange(&holderShutdownRequested, 0)) {
    // This runs only after the parent has observed EBUSY from a fresh
    // contender. It is ordinary document cleanup, not an orderly lease-release
    // acknowledgement after the intentionally failed terminal drain.
    exit(0);
  }
#endif
}

int main(void) {
#if defined(WASMFS_OPFS_DIRECT_PROXY_COMPLETION_HOLDER)
  const int role = kHolder;
  const int error = RunHolder();
#else
  const int role = kContender;
  const int error = RunContender();
#endif
  const int result = error == 0 ? kReady : error == EBUSY ? kBusy : kFailure;
  ReportResult(role, result, error);

  // The page starts its contender only after this holder has completed its
  // failed terminal drain. Keeping the holder document alive makes EBUSY a
  // direct observation of the retained lease, not an inference from iframe
  // teardown or browser-context cleanup.
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
