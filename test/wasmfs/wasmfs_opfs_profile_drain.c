/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

enum TestRole {
  kHolder,
  kContender,
};

enum TestResult {
  kReady,
  kBusy,
  kFailure,
};

#if defined(WASMFS_OPFS_PROFILE_DRAIN_NORMAL)
static const char kProfileName[] = "wasmfs-opfs-profile-drain-normal";
static const char kMountPath[] = "/profile-drain-normal";
static const char kMarkerPath[] = "/profile-drain-normal/marker";
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_NO_MOUNT)
static const char kProfileName[] = "wasmfs-opfs-profile-drain-no-mount";
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_CLOSE_FAILURE)
static const char kProfileName[] = "wasmfs-opfs-profile-drain-close-failure";
static const char kMountPath[] = "/profile-drain-close";
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_CLOSE_BEFORE)
static const char kProfileName[] = "wasmfs-opfs-profile-drain-close-before";
static const char kMountPath[] = "/profile-drain-close-before";
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_RELEASE_FAILURE)
static const char kProfileName[] =
  "wasmfs-opfs-profile-drain-release-failure";
static const char kMountPath[] = "/profile-drain-release-failure";
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_REENTRY)
static const char kProfileName[] = "wasmfs-opfs-profile-drain-reentry";
static const char kMountPath[] = "/profile-drain-reentry";
#else
#error "select one scoped profile drain scenario"
#endif

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

#if defined(WASMFS_OPFS_PROFILE_DRAIN_NORMAL) && \
  defined(WASMFS_OPFS_PROFILE_DRAIN_HOLDER)
static _Atomic int normal_holder_shutdown_requested;
static _Atomic int normal_holder_progress;

// Diagnostic-only test export. It is confined to this focused holder fixture
// and makes a browser timeout identify whether the deterministic barrier or a
// prior bootstrap step stopped progressing.
EMSCRIPTEN_KEEPALIVE
int wasmfs_opfs_profile_drain_holder_progress(void) {
  return atomic_load(&normal_holder_progress);
}

// This runs after the scoped drain and after the parent has tested a fresh
// contender while this holder remains live. It is a native atexit witness that
// unrelated WasmFS paths stay usable through the ordinary runtime exit tail.
static void NormalHolderAtexit(void) {
  int fd = open("/tmp/scoped-profile-drain-atexit",
                O_CREAT | O_TRUNC | O_WRONLY,
                0600);
  if (fd < 0 || write(fd, "z", 1) != 1 || close(fd) != 0) {
    abort();
  }
}

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_profile_drain_holder_shutdown(void) {
  atomic_store(&normal_holder_shutdown_requested, 1);
}

static void ExitNormalHolderWhenRequested(void) {
  if (atomic_exchange(&normal_holder_shutdown_requested, 0)) {
    exit(0);
  }
}
#endif

static void ExpectErrno(int result, int expected) {
  assert(result == -1);
  assert(errno == expected);
}

static void ReportResultOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-drain',
      },
      window.location.origin);
  }, role, result, error);
}

static void ReportResult(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportResultOnBrowserThread, role, result, error);
}

static backend_t AcquireLeasedBackend(void) {
  errno = 0;
  backend_t backend = wasmfs_create_opfs_backend_with_profile_lease(kProfileName);
  if (!backend) {
    return NULL;
  }
  return backend;
}

#if defined(WASMFS_OPFS_PROFILE_DRAIN_NORMAL)

// These private test hooks exist only in the
// WASMFS_OPFS_PROFILE_DRAIN_TEST libwasmfs variation. They create a precise
// pre-seal operation and an after-seal/before-detach observation window.
extern void wasmfs_opfs_profile_drain_test_arm_after_seal(void);
extern int wasmfs_opfs_profile_drain_test_after_seal_state(void);
extern void wasmfs_opfs_profile_drain_test_continue_after_seal(void);
extern void wasmfs_opfs_profile_drain_test_arm_file_operation(void);
extern int wasmfs_opfs_profile_drain_test_file_operation_state(void);
extern void wasmfs_opfs_profile_drain_test_continue_file_operation(void);
extern void wasmfs_opfs_profile_drain_test_reset_sealing(void);
extern int wasmfs_opfs_profile_drain_test_sealing_state(void);

struct WriteThread {
  int fd;
  ssize_t result;
};

struct DrainThread {
  backend_t backend;
  int result;
  wasmfs_opfs_profile_drain_result details;
};

struct TerminalDrainThread {
  int result;
  wasmfs_terminal_drain_result details;
};

static void* WriteWhileSealing(void* arg) {
  struct WriteThread* state = arg;
  // A positioned write does not disturb the still-buffered marker stream.
  state->result = pwrite(state->fd, "!", 1, 1024);
  return NULL;
}

static void* DrainProfile(void* arg) {
  struct DrainThread* state = arg;
  state->result =
    wasmfs_drain_opfs_profile_backend(state->backend, &state->details);
  return NULL;
}

static void* AttemptTerminalDrain(void* arg) {
  struct TerminalDrainThread* state = arg;
  state->result = wasmfs_terminal_drain(&state->details);
  return NULL;
}

static void WaitForTestState(int (*get_state)(void), int expected) {
  for (int i = 0; i < 10000; ++i) {
    if (get_state() == expected) {
      return;
    }
    emscripten_thread_sleep(1);
  }
  assert(!"timed out waiting for scoped profile drain test hook");
}

static int RunNormalHolder(backend_t backend) {
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  assert(atexit(NormalHolderAtexit) == 0);
  atomic_store(&normal_holder_progress, 1);
#endif
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  atomic_store(&normal_holder_progress, 2);
#endif

  int data = open(kMarkerPath, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(data >= 0);
  FILE* marker = fdopen(data, "w");
  assert(marker);
  static char marker_buffer[BUFSIZ];
  assert(setvbuf(marker, marker_buffer, _IOFBF, sizeof(marker_buffer)) == 0);
  static const char kMarker[] = "scoped profile drain marker";
  assert(fputs(kMarker, marker) >= 0);

  int alias = dup(data);
  assert(alias >= 0);
  int directory = open(kMountPath, O_RDONLY | O_DIRECTORY);
  assert(directory >= 0);
  assert(chdir(kMountPath) == 0);

  // The writer has acquired the target backend token before its OPFS virtual
  // call. It remains blocked there while the drain seals and waits for it.
  wasmfs_opfs_profile_drain_test_arm_file_operation();
  struct WriteThread writer = {.fd = alias, .result = -1};
  pthread_t writer_thread;
  assert(pthread_create(&writer_thread, NULL, WriteWhileSealing, &writer) ==
         0);
  WaitForTestState(wasmfs_opfs_profile_drain_test_file_operation_state, 2);
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  atomic_store(&normal_holder_progress, 3);
#endif

  wasmfs_opfs_profile_drain_test_reset_sealing();
  wasmfs_opfs_profile_drain_test_arm_after_seal();
  struct DrainThread drain = {.backend = backend, .result = -1, .details = {0}};
  pthread_t drain_thread;
  assert(pthread_create(&drain_thread, NULL, DrainProfile, &drain) == 0);
  WaitForTestState(wasmfs_opfs_profile_drain_test_sealing_state, 1);
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  atomic_store(&normal_holder_progress, 4);
#endif

  // The target descriptor is still installed but its backend is sealed, so
  // metadata access must expose ESHUTDOWN rather than an invented stat value.
  struct stat buffer;
  // The writer deliberately owns the data-file mutex at this point. A
  // separate target directory descriptor proves sealed metadata rejection
  // without waiting on that pre-admitted I/O operation.
  errno = 0;
  ExpectErrno(fstat(directory, &buffer), ESHUTDOWN);

  // Let the pre-admitted write pass through the OPFS virtual call. A broken
  // pre-seal token implementation would reject it after the state transition.
  wasmfs_opfs_profile_drain_test_continue_file_operation();
  assert(pthread_join(writer_thread, NULL) == 0);
  assert(writer.result == 1);

  // The drain has now sealed and paused before filtered descriptor detach.
  WaitForTestState(wasmfs_opfs_profile_drain_test_after_seal_state, 2);
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  atomic_store(&normal_holder_progress, 5);
#endif
  errno = 0;
  ExpectErrno(fstat(data, &buffer), ESHUTDOWN);
  errno = 0;
  assert(open("relative-profile-path", O_CREAT | O_RDWR, 0600) == -1);
  assert(errno == ESHUTDOWN);
  char cwd[PATH_MAX];
  errno = 0;
  assert(getcwd(cwd, sizeof(cwd)) == NULL);
  assert(errno == ESHUTDOWN);
  // The legacy JS bridge cannot return errno directly, but it must reject the
  // sealed CWD instead of fabricating a pathname.
  assert(EM_ASM_INT({
           try {
             FS.cwd();
             return 0;
           } catch (error) {
             return error.errno || 0;
           }
         }) == EIO);
  assert(wasmfs_unmount(kMountPath) == -ESHUTDOWN);
  assert(wasmfs_unmount(kMarkerPath) == -ESHUTDOWN);
  assert(wasmfs_create_file("/tmp/scoped-profile-explicit", 0600, backend) ==
         -ESHUTDOWN);
  errno = 0;
  assert(wasmfs_create_icase_backend(backend) == NULL);
  assert(errno == ESHUTDOWN);
  errno = 0;
  assert(wasmfs_get_backend_by_path(kMountPath) == NULL);
  assert(errno == ESHUTDOWN);
  assert(EM_ASM_INT({
           try {
             FS.findObject(UTF8ToString($0));
             return 0;
           } catch (error) {
             return error.errno || 0;
           }
         },
         kMountPath) == ESHUTDOWN);

  // An absolute unrelated path must remain usable even though CWD is sealed.
  int tmp = open("/tmp/scoped-profile-drain-live", O_CREAT | O_TRUNC | O_RDWR,
                 0600);
  assert(tmp >= 0);
  assert(write(tmp, "x", 1) == 1);
  assert(close(tmp) == 0);
  assert(chdir("/tmp") == 0);

  // Scoped profile drain and global terminal drain are mutually exclusive.
  // The zeroed result proves the global path did not begin a terminal drain
  // before it observed the scoped barrier.
  struct TerminalDrainThread terminal = {.result = 0, .details = {0}};
  pthread_t terminal_thread;
  assert(pthread_create(
           &terminal_thread, NULL, AttemptTerminalDrain, &terminal) == 0);
  assert(pthread_join(terminal_thread, NULL) == 0);
  assert(terminal.result == -EBUSY);
  assert(terminal.details.error == 0);

  wasmfs_opfs_profile_drain_test_continue_after_seal();
  assert(pthread_join(drain_thread, NULL) == 0);

  // Aggregate fflush(NULL) wrote the buffered marker before the target table
  // entries were detached. Aliases and directory descriptors then become
  // ordinary EBADF rather than remaining sealed pseudo-descriptors.
  assert(drain.result == 0);
  assert(drain.details.error == 0);
  assert(drain.details.detached_descriptors == 3);
  assert(drain.details.data_file_states == 1);
  assert(drain.details.libc_flush_failed == 0);
  assert(drain.details.data_flush_failures == 0);
  assert(drain.details.data_close_failures == 0);
  assert(drain.details.prior_close_failures == 0);
  assert(drain.details.lease_release_failures == 0);
  assert(drain.details.backend_sealed == 1);
  assert(drain.details.lease_released == 1);
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  atomic_store(&normal_holder_progress, 6);
#endif
  errno = 0;
  ExpectErrno(fstat(data, &buffer), EBADF);
  errno = 0;
  assert(fcntl(alias, F_GETFL) == -1);
  assert(errno == EBADF);
  errno = 0;
  ExpectErrno(fstat(directory, &buffer), EBADF);
  errno = 0;
  assert(fclose(marker) == EOF);
  assert(errno == EBADF);

  // Unlike terminalDrain, this leaves unrelated filesystem and stdio paths
  // live for the rest of the application and JavaScript exit tail.
  static const char kStdoutMarker[] =
    "scoped profile drain stdout remains live\n";
  assert(write(STDOUT_FILENO, kStdoutMarker, sizeof(kStdoutMarker) - 1) ==
         sizeof(kStdoutMarker) - 1);
  int post_drain_tmp =
    open("scoped-profile-drain-post", O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(post_drain_tmp >= 0);
  assert(write(post_drain_tmp, "y", 1) == 1);
  assert(close(post_drain_tmp) == 0);

  errno = 0;
  assert(open(kMarkerPath, O_RDONLY) == -1);
  assert(errno == ESHUTDOWN);

  wasmfs_opfs_profile_drain_result again = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &again) == -ESHUTDOWN);
  assert(again.error == -ESHUTDOWN);
  return 0;
}

static int RunNormalContender(backend_t backend) {
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);
  int fd = open(kMarkerPath, O_RDONLY);
  assert(fd >= 0);
  char marker[64] = {};
  static const char kExpected[] = "scoped profile drain marker";
  assert(read(fd, marker, sizeof(kExpected) - 1) == sizeof(kExpected) - 1);
  assert(memcmp(marker, kExpected, sizeof(kExpected) - 1) == 0);
  assert(close(fd) == 0);
  return 0;
}

#elif defined(WASMFS_OPFS_PROFILE_DRAIN_NO_MOUNT)

static int RunNoMountHolder(backend_t backend) {
  // The scoped primitive is also the cleanup path for a lease whose mount
  // setup failed before it reached the descriptor table.
  wasmfs_opfs_profile_drain_result invalid = {0};
  assert(wasmfs_drain_opfs_profile_backend(NULL, &invalid) == -EINVAL);
  assert(invalid.error == -EINVAL);

  backend_t default_opfs = wasmfs_create_opfs_backend();
  assert(default_opfs);
  wasmfs_opfs_profile_drain_result unsupported = {0};
  assert(wasmfs_drain_opfs_profile_backend(default_opfs, &unsupported) ==
         -ENOTSUP);
  assert(unsupported.error == -ENOTSUP);

  wasmfs_opfs_profile_drain_result drain = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &drain) == 0);
  assert(drain.error == 0);
  assert(drain.detached_descriptors == 0);
  assert(drain.data_file_states == 0);
  assert(drain.backend_sealed == 1);
  assert(drain.lease_released == 1);
  return 0;
}

static int RunNoMountContender(backend_t backend) {
  assert(wasmfs_create_directory("/profile-drain-no-mount", 0700, backend) ==
         0);
  return 0;
}

#elif defined(WASMFS_OPFS_PROFILE_DRAIN_CLOSE_FAILURE)

static int RunCloseFailureHolder(backend_t backend) {
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);
  int fd = open("/profile-drain-close/file", O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(fd >= 0);
  assert(write(fd, "x", 1) == 1);

  wasmfs_opfs_profile_drain_result drain = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &drain) == -EIO);
  assert(drain.error == -EIO);
  assert(drain.detached_descriptors == 1);
  assert(drain.data_file_states == 1);
  assert(drain.libc_flush_failed == 0);
  assert(drain.data_flush_failures == 0);
  assert(drain.data_close_failures == 1);
  assert(drain.prior_close_failures == 0);
  assert(drain.lease_release_failures == 0);
  assert(drain.backend_sealed == 1);
  assert(drain.lease_released == 0);
  struct stat buffer;
  errno = 0;
  ExpectErrno(fstat(fd, &buffer), EBADF);
  return 0;
}

#elif defined(WASMFS_OPFS_PROFILE_DRAIN_CLOSE_BEFORE)

static int RunCloseBeforeHolder(backend_t backend) {
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);
  int fd =
    open("/profile-drain-close-before/file", O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(fd >= 0);
  assert(write(fd, "x", 1) == 1);

  // The injected close fails before browser close(), but POSIX close removes
  // the descriptor. The scoped drain must observe the backend latch rather
  // than treating an empty target table as proof that release is safe.
  errno = 0;
  assert(close(fd) == -1);
  assert(errno == EIO);

  wasmfs_opfs_profile_drain_result drain = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &drain) == -EIO);
  assert(drain.error == -EIO);
  assert(drain.detached_descriptors == 0);
  assert(drain.data_file_states == 0);
  assert(drain.libc_flush_failed == 0);
  assert(drain.data_flush_failures == 0);
  assert(drain.data_close_failures == 0);
  assert(drain.prior_close_failures == 1);
  assert(drain.lease_release_failures == 0);
  assert(drain.backend_sealed == 1);
  assert(drain.lease_released == 0);

  // A later global terminal drain must report the previously failed scoped
  // handoff rather than claiming success while this holder still retains its
  // profile lease. It must not retry the ambiguous release.
  wasmfs_terminal_drain_result terminal = {0};
  assert(wasmfs_terminal_drain(&terminal) == -ESHUTDOWN);
  assert(terminal.error == -ESHUTDOWN);
  assert(terminal.backend_terminal_failures == 1);
  return 0;
}

#elif defined(WASMFS_OPFS_PROFILE_DRAIN_RELEASE_FAILURE)

static int RunReleaseFailureHolder(backend_t backend) {
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);

  wasmfs_opfs_profile_drain_result drain = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &drain) == -EIO);
  assert(drain.error == -EIO);
  assert(drain.detached_descriptors == 0);
  assert(drain.data_file_states == 0);
  assert(drain.libc_flush_failed == 0);
  assert(drain.data_flush_failures == 0);
  assert(drain.data_close_failures == 0);
  assert(drain.prior_close_failures == 0);
  assert(drain.lease_release_failures == 1);
  assert(drain.backend_sealed == 1);
  assert(drain.lease_released == 0);

  wasmfs_opfs_profile_drain_result again = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &again) == -ESHUTDOWN);
  assert(again.error == -ESHUTDOWN);
  return 0;
}

#elif defined(WASMFS_OPFS_PROFILE_DRAIN_REENTRY)

struct CookieState {
  int calls;
  int error;
};

static ssize_t ReentrantCookieWrite(void* cookie,
                                    const char* buffer,
                                    size_t length) {
  struct CookieState* state = cookie;
  (void)buffer;
  (void)length;
  ++state->calls;
  errno = 0;
  assert(open("/profile-drain-reentry/reentrant", O_CREAT | O_RDWR, 0600) ==
         -1);
  state->error = errno;
  return -1;
}

static int RunReentryHolder(backend_t backend) {
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);
  struct CookieState state = {0};
  cookie_io_functions_t callbacks = {0};
  callbacks.write = ReentrantCookieWrite;
  FILE* cookie = fopencookie(&state, "w", callbacks);
  assert(cookie);
  assert(fputs("reentry", cookie) >= 0);

  wasmfs_opfs_profile_drain_result drain = {0};
  assert(wasmfs_drain_opfs_profile_backend(backend, &drain) == -ESHUTDOWN);
  assert(drain.error == -ESHUTDOWN);
  assert(drain.libc_flush_failed == 1);
  assert(drain.data_file_states == 0);
  assert(drain.backend_sealed == 1);
  assert(drain.lease_released == 0);
  assert(state.calls == 1);
  assert(state.error == ESHUTDOWN);
  (void)fclose(cookie);
  return 0;
}

#endif

static int RunHolder(void) {
  backend_t backend = AcquireLeasedBackend();
  if (!backend) {
    return ErrorOrEIO();
  }
#if defined(WASMFS_OPFS_PROFILE_DRAIN_NORMAL)
  return RunNormalHolder(backend);
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_NO_MOUNT)
  return RunNoMountHolder(backend);
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_CLOSE_FAILURE)
  return RunCloseFailureHolder(backend);
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_CLOSE_BEFORE)
  return RunCloseBeforeHolder(backend);
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_RELEASE_FAILURE)
  return RunReleaseFailureHolder(backend);
#else
  return RunReentryHolder(backend);
#endif
}

static int RunContender(void) {
  backend_t backend = AcquireLeasedBackend();
  if (!backend) {
    return ErrorOrEIO();
  }
#if defined(WASMFS_OPFS_PROFILE_DRAIN_NORMAL)
  return RunNormalContender(backend);
#elif defined(WASMFS_OPFS_PROFILE_DRAIN_NO_MOUNT)
  return RunNoMountContender(backend);
#else
  // A close or reentrant-stream failure must retain the Web Lock while this
  // holder remains live. Reaching this line would be a false success.
  return EIO;
#endif
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  assert(!emscripten_is_main_runtime_thread());
  int error;
#ifdef WASMFS_OPFS_PROFILE_DRAIN_HOLDER
  error = RunHolder();
  ReportResult(kHolder, error == 0 ? kReady : kFailure, error);
#else
  error = RunContender();
  ReportResult(kContender,
               error == 0 ? kReady : error == EBUSY ? kBusy : kFailure,
               error);
#endif
#if defined(WASMFS_OPFS_PROFILE_DRAIN_NORMAL) && \
  defined(WASMFS_OPFS_PROFILE_DRAIN_HOLDER)
  emscripten_set_main_loop(ExitNormalHolderWhenRequested, 0, false);
#else
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#endif
}
