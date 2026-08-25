// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This browser-coordinated regression establishes a populated namespace,
// then starts fresh modules whose test-only native parser rejects one
// successfully-read permanent journal slot. Each corruptor must fail the
// factory with EIO and release its lease. A final uninstrumented module reads
// the original sentinel, proving the failure path did not reset or adopt the
// established profile. The test does not mutate raw OPFS bytes and is not a
// browser-crash, power-loss, database, or complete Chromium-profile proof.

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_CORRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_VERIFIER)
#error "select an OPFS profile namespace journal corruption role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_CORRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_VERIFIER) != 1
#error "select exactly one OPFS profile namespace journal corruption role"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_PROFILE_NAME
#error "select an OPFS profile namespace journal corruption profile name"
#endif

#define WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_STRINGIFY_IMPL(value)

enum TestRole {
  kOwner,
  kCorruptor,
  kVerifier,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_STRINGIFY(
    WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_PROFILE_NAME);
static const char kMountPath[] = "/profile";
static const char kSentinelPath[] = "/profile/sentinel";
static const char kSentinel[] = "established-profile-sentinel";

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-namespace-journal-corruption',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

static int MountNamespace(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_namespace_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(kMountPath, 0700, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainNamespace(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired
           ? 0
           : EIO;
}

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_OWNER)
static int WriteSentinel(void) {
  const int fd = open(kSentinelPath, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = 0;
  if (write(fd, kSentinel, strlen(kSentinel)) != (ssize_t)strlen(kSentinel)) {
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

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_VERIFIER)
static int VerifySentinel(void) {
  const int fd = open(kSentinelPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  char actual[sizeof(kSentinel)] = {};
  int error = 0;
  if (read(fd, actual, sizeof(actual)) != (ssize_t)strlen(kSentinel) ||
      memcmp(actual, kSentinel, strlen(kSentinel)) != 0) {
    error = EIO;
  }
  if (close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  return error;
}
#endif

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_OWNER)
  error = MountNamespace(&backend);
  if (error == 0) {
    error = WriteSentinel();
  }
  if (backend) {
    const int drainError = DrainNamespace(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kOwner, error);
#elif defined(WASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_CORRUPTOR)
  errno = 0;
  backend = wasmfs_create_opfs_profile_namespace_backend(kProfileName);
  if (backend || errno != EIO) {
    error = EIO;
  }
  // The expected factory failure completes its own lease handoff. If a future
  // regression unexpectedly returns a backend, drain it before reporting so
  // this failing test cannot strand the profile lease.
  if (backend) {
    const int drainError = DrainNamespace(backend);
    if (drainError != 0) {
      error = drainError;
    }
  }
  Report(kCorruptor, error);
#else
  error = MountNamespace(&backend);
  if (error == 0) {
    error = VerifySentinel();
  }
  if (backend) {
    const int drainError = DrainNamespace(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kVerifier, error);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
