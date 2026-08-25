// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This browser-coordinated regression keeps profile "collision" in its
// unexposed PREPARED bootstrap state while profile "collision.bootstrap"
// starts. The profiles use different Web Locks, so a suffix-derived physical
// naming scheme could otherwise let the latter interpret one of the former's
// bootstrap artifacts as its own canonical profile. Once both profiles have
// mounted and written distinct sentinels, each performs an explicit
// result-bearing scoped drain; fresh modules then reopen and verify the two
// independent states.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_VERIFIER)
#error "select a profile-name isolation test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER) + \
      defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_VERIFIER) != 1
#error "select exactly one profile-name isolation test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PAUSE_BOOTSTRAP) && \
  !defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER)
#error "only a holder may pause at the bootstrap selector"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PROFILE_NAME
#error "select a profile name"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_SENTINEL
#error "select a profile sentinel"
#endif

#ifndef WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PROFILE_ROLE
#error "select a profile role"
#endif

#define WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_STRINGIFY_IMPL(value)

enum TestEvent {
  // The first profile's private container has a flushed payload, but has not
  // yet made a selector durable.  It must remain distinct from the second
  // profile's published physical name.
  kBootstrapReady,
  kReady,
  kDrained,
  kVerified,
};

static const int kProfileRole =
  WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PROFILE_ROLE;
static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_STRINGIFY(
    WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PROFILE_NAME);
static const char kSentinel[] =
  WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_STRINGIFY(
    WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_SENTINEL);
static const char kMountPath[] = "/profile";
static const char kSentinelPath[] = "/profile/sentinel";

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int event, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-namespace-profile-name-isolation',
      },
      window.location.origin);
  }, role, event, error);
}

static void Report(int event, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, kProfileRole, event, error);
}

static int CreateAndMountNamespace(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_namespace_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }

  const int result = wasmfs_create_directory(kMountPath, 0700, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER)
static int WriteSentinel(void) {
  int fd = open(kSentinelPath, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }

  const size_t size = strlen(kSentinel);
  int error = 0;
  if (write(fd, kSentinel, size) != (ssize_t)size) {
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

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_VERIFIER)
static int VerifySentinel(void) {
  int fd = open(kSentinelPath, O_RDONLY);
  if (fd < 0) {
    return ErrorOrEIO();
  }

  char actual[128] = {};
  const size_t expectedSize = strlen(kSentinel);
  int error = 0;
  if (expectedSize >= sizeof(actual) ||
      read(fd, actual, sizeof(actual)) != (ssize_t)expectedSize ||
      memcmp(actual, kSentinel, expectedSize) != 0) {
    error = EIO;
  }
  if (close(fd) != 0 && error == 0) {
    error = ErrorOrEIO();
  }
  return error;
}
#endif

struct ScopedDrainState {
  backend_t backend;
  int result;
  wasmfs_opfs_profile_drain_result details;
};

static void* RunScopedDrain(void* opaque) {
  struct ScopedDrainState* state = opaque;
  state->result = wasmfs_drain_opfs_profile_backend(
    state->backend, &state->details);
  return NULL;
}

// The public drain ABI deliberately rejects the runtime-main and browser-main
// threads. Run it on a separately-created application pthread, then validate
// the complete handoff result rather than inferring release from iframe life.
static int DrainScoped(backend_t backend) {
  struct ScopedDrainState state = {backend, EIO, {0}};
  pthread_t thread;
  int result = pthread_create(&thread, NULL, RunScopedDrain, &state);
  if (result != 0) {
    return result;
  }
  result = pthread_join(thread, NULL);
  if (result != 0) {
    return result;
  }
  return state.result == 0 && state.details.error == 0 &&
             state.details.backend_sealed && state.details.lease_released &&
             state.details.backend_retired
           ? 0
           : EIO;
}

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PAUSE_BOOTSTRAP)
static _Atomic int bootstrapPaused;
static _Atomic int resumeBootstrap;

// This test-only bridge is linked only into the first holder variation. The
// matching libwasmfs variation calls it immediately before selector writes.
// Pause only the initial empty-root commit; later sentinel commits must run.
void wasmfs_opfs_profile_namespace_test_maybe_interrupt_selector(int phase) {
  if (phase != 1 || atomic_exchange(&bootstrapPaused, 1)) {
    return;
  }
  Report(kBootstrapReady, 0);
  while (!atomic_load(&resumeBootstrap)) {
    emscripten_thread_sleep(10);
  }
}

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_profile_namespace_isolation_resume_bootstrap(void) {
  atomic_store(&resumeBootstrap, 1);
}
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER)
static backend_t holderBackend;
static _Atomic int drainRequested;
static _Atomic int drainReported;

// This export runs only an atomic request on the browser main thread. The
// holder's application thread observes it from its event loop and performs
// the result-bearing drain on a separate application pthread.
EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_profile_namespace_isolation_request_drain(void) {
  atomic_store(&drainRequested, 1);
}

static void HolderMainLoop(void) {
  if (!atomic_exchange(&drainRequested, 0) ||
      atomic_exchange(&drainReported, 1)) {
    return;
  }
  Report(kDrained, DrainScoped(holderBackend));
}
#endif

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_VERIFIER)
static void KeepRuntimeAlive(void) {}
#endif

int main(void) {
  backend_t backend = NULL;
  int error = CreateAndMountNamespace(&backend);

#if defined(WASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER)
  if (error == 0) {
    error = WriteSentinel();
  }
  if (error == 0) {
    holderBackend = backend;
  }
  Report(kReady, error);
  emscripten_set_main_loop(HolderMainLoop, 0, false);
#else
  if (error == 0) {
    error = VerifySentinel();
  }
  if (error == 0) {
    error = DrainScoped(backend);
  }
  Report(kVerified, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#endif
}
