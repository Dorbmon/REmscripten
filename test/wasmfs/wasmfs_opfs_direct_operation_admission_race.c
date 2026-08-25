// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// Verify that a direct OPFS operation retains its ambiguity admission until it
// receives a native proxy completion. The phase-2 system-lib variation pauses
// the latcher after admission and before its proxy callback, then deliberately
// loses that callback's completion acknowledgement. A second operation against
// a different directory must be blocked at the shared backend gate and later
// rejected with EIO without creating its child.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#ifndef WASMFS_OPFS_DIRECT_ADMISSION_RACE_MOUNT
#error "select a unique direct OPFS admission-race mount name"
#endif

#define WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY(value) \
  WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY_IMPL(value)

enum DirectOperationAdmissionRaceState {
  kRaceIdle,
  kRaceArmed,
  kRaceLatcherPaused,
  kRaceFollowerBlocked,
  kRaceFollowerBypassed,
  kRaceContinue,
};

extern void wasmfs_opfs_direct_operation_admission_race_test_arm(void);
extern int wasmfs_opfs_direct_operation_admission_race_test_state(void);
extern void wasmfs_opfs_direct_operation_admission_race_test_continue(void);
extern void wasmfs_opfs_direct_operation_admission_race_test_reset(void);

static const char kMountPath[] =
  "/" WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY(
         WASMFS_OPFS_DIRECT_ADMISSION_RACE_MOUNT);
static const char kCleanupMountPath[] =
  "/" WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY(
         WASMFS_OPFS_DIRECT_ADMISSION_RACE_MOUNT) "-cleanup";
// Direct OPFS exposes the origin's physical root at every virtual mountpoint.
// Derive physical children from this test's nonce-bearing mount name rather
// than treating the virtual path as physical isolation.
static const char kFirstDirectoryLeaf[] =
  WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY(
    WASMFS_OPFS_DIRECT_ADMISSION_RACE_MOUNT) "-first";
static const char kSecondDirectoryLeaf[] =
  WASMFS_OPFS_DIRECT_ADMISSION_RACE_STRINGIFY(
    WASMFS_OPFS_DIRECT_ADMISSION_RACE_MOUNT) "-second";
static const char kLatcherLeaf[] =
  "__wasmfs_opfs_test_direct_operation_admission_latcher__";
static const char kFollowerLeaf[] = "follower";

static char latcherPath[PATH_MAX];
static char followerPath[PATH_MAX];
static _Atomic int latcherError;
static _Atomic int followerError;
static _Atomic int controllerResult = -1;
static _Atomic int controllerReported;

static void makePath(char* output,
                     size_t outputSize,
                     const char* parent,
                     const char* child) {
  const int written = snprintf(output, outputSize, "%s/%s", parent, child);
  assert(written >= 0 && (size_t)written < outputSize);
}

static int openForCreation(const char* path) {
  errno = 0;
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) {
    return errno;
  }
  assert(close(fd) == 0);
  return 0;
}

static void* createLatcher(void* unused) {
  atomic_store(&latcherError, openForCreation(latcherPath));
  return NULL;
}

static void* createFollower(void* unused) {
  atomic_store(&followerError, openForCreation(followerPath));
  return NULL;
}

static int waitForState(int first, int second) {
  int state = wasmfs_opfs_direct_operation_admission_race_test_state();
  for (int attempts = 0; attempts < 10000; ++attempts) {
    if (state == first || state == second) {
      return state;
    }
    emscripten_thread_sleep(1);
    state = wasmfs_opfs_direct_operation_admission_race_test_state();
  }
  return state;
}

static void removeFileIfPresent(const char* path) {
  errno = 0;
  assert(unlink(path) == 0 || errno == ENOENT);
}

static int runRace(void) {
  char firstDirectory[PATH_MAX];
  char secondDirectory[PATH_MAX];
  char cleanupFirstDirectory[PATH_MAX];
  char cleanupSecondDirectory[PATH_MAX];
  char cleanupLatcherPath[PATH_MAX];
  char cleanupFollowerPath[PATH_MAX];
  makePath(firstDirectory,
           sizeof(firstDirectory),
           kMountPath,
           kFirstDirectoryLeaf);
  makePath(secondDirectory,
           sizeof(secondDirectory),
           kMountPath,
           kSecondDirectoryLeaf);
  makePath(latcherPath, sizeof(latcherPath), firstDirectory, kLatcherLeaf);
  makePath(followerPath, sizeof(followerPath), secondDirectory, kFollowerLeaf);

  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);
  assert(mkdir(firstDirectory, 0700) == 0);
  assert(mkdir(secondDirectory, 0700) == 0);

  atomic_store(&latcherError, -1);
  atomic_store(&followerError, -1);
  wasmfs_opfs_direct_operation_admission_race_test_arm();

  pthread_t latcher;
  pthread_t follower;
  assert(pthread_create(&latcher, NULL, createLatcher, NULL) == 0);
  assert(waitForState(kRaceLatcherPaused, kRaceLatcherPaused) ==
         kRaceLatcherPaused);
  assert(pthread_create(&follower, NULL, createFollower, NULL) == 0);

  // The test hook calls try_lock while the latcher is paused. A bypass means
  // the second operation was not stopped at the direct backend's admission
  // mutex; keep both threads releasable so the failure is reported cleanly.
  const int raceState = waitForState(
    kRaceFollowerBlocked, kRaceFollowerBypassed);
  if (raceState == kRaceLatcherPaused || raceState == kRaceFollowerBlocked ||
      raceState == kRaceFollowerBypassed) {
    wasmfs_opfs_direct_operation_admission_race_test_continue();
  }
  assert(pthread_join(latcher, NULL) == 0);
  assert(pthread_join(follower, NULL) == 0);
  wasmfs_opfs_direct_operation_admission_race_test_reset();

  // The failed backend cannot safely inspect or clean up the physical side
  // effect of its latcher. Use a fresh direct backend to confirm that the
  // follower did not create anything, then remove only this fixture's names.
  backend_t cleanupBackend = wasmfs_create_opfs_backend();
  assert(cleanupBackend);
  assert(wasmfs_create_directory(kCleanupMountPath, 0700, cleanupBackend) ==
         0);
  makePath(cleanupFirstDirectory,
           sizeof(cleanupFirstDirectory),
           kCleanupMountPath,
           kFirstDirectoryLeaf);
  makePath(cleanupSecondDirectory,
           sizeof(cleanupSecondDirectory),
           kCleanupMountPath,
           kSecondDirectoryLeaf);
  makePath(cleanupLatcherPath,
           sizeof(cleanupLatcherPath),
           cleanupFirstDirectory,
           kLatcherLeaf);
  makePath(cleanupFollowerPath,
           sizeof(cleanupFollowerPath),
           cleanupSecondDirectory,
           kFollowerLeaf);
  errno = 0;
  const int followerAbsent =
    access(cleanupFollowerPath, F_OK) == -1 && errno == ENOENT;
  removeFileIfPresent(cleanupLatcherPath);
  removeFileIfPresent(cleanupFollowerPath);
  assert(rmdir(cleanupFirstDirectory) == 0);
  assert(rmdir(cleanupSecondDirectory) == 0);

  // Always clean nonce-isolated physical entries before reporting a detected
  // admission bypass, so an intentional regression failure cannot disturb a
  // later browser test or a parallel test invocation.
  assert(raceState == kRaceFollowerBlocked);
  assert(atomic_load(&latcherError) == EIO);
  assert(atomic_load(&followerError) == EIO);
  assert(followerAbsent);
  return 0;
}

static void* runRaceOnControllerThread(void* unused) {
  atomic_store(&controllerResult, runRace());
  return NULL;
}

static void reportResultOnBrowserThread(int result) {
  EM_ASM({
    window.location.href = $0 == 0 ? '/report_result?0' :
                                     '/report_result?failure';
  }, result);
}

static void finishOnApplicationThread(void) {
  const int result = atomic_load(&controllerResult);
  if (result < 0 || atomic_exchange(&controllerReported, 1)) {
    return;
  }
  // The application main loop is itself proxied to a pthread. Send the
  // result request through browser main rather than touching `window` here.
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VI, reportResultOnBrowserThread, result);
}

int main(void) {
  pthread_t controller;
  assert(pthread_create(&controller, NULL, runRaceOnControllerThread, NULL) ==
         0);
  assert(pthread_detach(controller) == 0);
  emscripten_set_main_loop(finishOnApplicationThread, 0, true);
}
