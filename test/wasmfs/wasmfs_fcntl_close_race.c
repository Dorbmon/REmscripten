// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// Regression test for a descriptor-table snapshot racing with close(). The
// private hook fires after fcntl has normalized the old OpenFileState, but
// before it can publish the lock. Closing and then dup2()-reinstalling the
// same OpenFileState at that descriptor number must make the stale request
// fail with EBADF rather than resurrect a process-owned lock.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <emscripten/threading.h>

// This private test-only symbol is compiled only with
// -sWASMFS_RECORD_LOCK_TEST=1. F_GETLK hides locks owned by the calling
// process, so the counter is the only way to observe a stale lock directly.
extern int wasmfs_record_lock_count_for_testing(int fd);

enum FcntlCloseRaceState {
  kIdle,
  kArmed,
  kPausedBeforeRecheck,
  kRelease,
};

static _Atomic int fcntlCloseRaceState;

// syscalls.cpp invokes this weak, internal test seam after normalizing a
// record-lock request and before it rechecks the descriptor table. It is
// intentionally a test-only synchronization barrier, not public WasmFS API.
void wasmfs_record_lock_before_recheck_for_testing(void) {
  int expected = kArmed;
  if (!atomic_compare_exchange_strong(
        &fcntlCloseRaceState, &expected, kPausedBeforeRecheck)) {
    return;
  }
  while (atomic_load(&fcntlCloseRaceState) == kPausedBeforeRecheck) {
    emscripten_thread_sleep(1);
  }
  atomic_store(&fcntlCloseRaceState, kIdle);
}

struct FcntlThread {
  int fd;
  int result;
  int error;
};

static void* SetRecordLock(void* argument) {
  struct FcntlThread* thread = argument;
  struct flock lock = {
    .l_type = F_WRLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 1,
    .l_pid = -1,
  };
  errno = 0;
  thread->result = fcntl(thread->fd, F_SETLK, &lock);
  thread->error = errno;
  return NULL;
}

static int WaitForPause(void) {
  for (int attempts = 0; attempts < 10000; ++attempts) {
    if (atomic_load(&fcntlCloseRaceState) == kPausedBeforeRecheck) {
      return 1;
    }
    emscripten_thread_sleep(1);
  }
  return 0;
}

static void TestCloseAndDup2WinsRecordLockRace(void) {
  const char path[] = "/wasmfs-fcntl-close-race";
  const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  const int alias = dup(fd);
  assert(alias >= 0);

  struct FcntlThread thread = {
    .fd = fd,
    .result = 0,
    .error = 0,
  };
  pthread_t pthread;
  atomic_store(&fcntlCloseRaceState, kArmed);
  assert(pthread_create(&pthread, NULL, SetRecordLock, &thread) == 0);

  const int paused = WaitForPause();
  // A missed hook means this focused regression has already failed. Do not
  // enter a blocking pthread_join() that could wait forever for a thread hung
  // before the hook; aborting the isolated test module is a bounded failure.
  if (!paused) {
    abort();
  }
  int closeResult = -1;
  int reinstalled = -1;
  if (paused) {
    closeResult = close(fd);
    if (closeResult == 0) {
      // Reinstall the exact same OpenFileState, not merely the same dcache
      // File. A pointer-only table recheck would accept this replacement even
      // though the intervening close already released every process lock.
      reinstalled = dup2(alias, fd);
    }
  }

  // The hook was reached, so release and join before asserting rather than
  // leaving a browser worker in the internal test barrier.
  atomic_store(&fcntlCloseRaceState, kRelease);
  assert(pthread_join(pthread, NULL) == 0);

  const int noStaleLock = reinstalled >= 0 &&
                          wasmfs_record_lock_count_for_testing(alias) == 0 &&
                          wasmfs_record_lock_count_for_testing(reinstalled) ==
                            0;
  if (reinstalled >= 0) {
    assert(close(reinstalled) == 0);
  }
  assert(close(alias) == 0);
  assert(unlink(path) == 0);

  assert(closeResult == 0);
  assert(reinstalled == fd);
  assert(thread.result == -1);
  assert(thread.error == EBADF);
  assert(noStaleLock);
}

int main(void) {
  TestCloseAndDup2WinsRecordLockRace();
  puts("success");
  return 0;
}
