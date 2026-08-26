// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document proof that a leased V4 profile supports Chromium's
// process-owned fcntl database-lock subset without exposing a second WasmFS
// profile instance while the browser Web Lock is held.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_HOLDER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_CONTENDER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_DRAINER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_VERIFIER)
#error "select one V4 filesystem lock test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_HOLDER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_CONTENDER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_DRAINER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_VERIFIER) != 1
#error "select exactly one V4 filesystem lock test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_PROFILE_NAME
#error "select a V4 filesystem lock test profile name"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_STRINGIFY_IMPL(value)

// Compiled only with -sWASMFS_RECORD_LOCK_TEST=1. F_GETLK intentionally hides
// locks owned by this WasmFS process, so this counter verifies POSIX's
// any-descriptor-close behavior directly.
extern int wasmfs_record_lock_count_for_testing(int fd);

// syscalls.cpp calls this private test hook after range normalization and
// before it rechecks the descriptor table. The pthread below deliberately
// pauses there so a close/reopen wins the table race deterministically.
static _Atomic int fcntlCloseRaceState;

void wasmfs_record_lock_before_recheck_for_testing(void) {
  int expected = 1;
  if (!atomic_compare_exchange_strong(&fcntlCloseRaceState, &expected, 2)) {
    return;
  }
  while (atomic_load(&fcntlCloseRaceState) == 2) {
    emscripten_thread_sleep(1);
  }
  atomic_store(&fcntlCloseRaceState, 0);
}

enum TestRole {
  kHolder,
  kContender,
  kDrainer,
  kVerifier,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-locks";
static const char kDataPath[] = "/v4fs-locks/database";
static const uint8_t kContents[] = {
  0x64, 0x61, 0x74, 0x61, 0x62, 0x61, 0x73, 0x65,
};

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error, int ready) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        event: $2 ? 'ready' : 'result',
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-locks',
      },
      window.location.origin);
  }, role, error, ready);
}

static void Report(int role, int error, int ready) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, error, ready);
}

static int MountFilesystem(backend_t* backend) {
  if (!backend) {
    return EINVAL;
  }
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(kMountPath, 0700, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainFilesystem(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

// Draining a live profile must safely detach an open, record-locked
// descriptor. In particular, a lock must not make the backend retain its Web
// Lock lease or leave a DataFile state behind. The iframe remains alive after
// this call so the parent can prove that a fresh iframe acquires the released
// lease rather than relying on document destruction.
static int DrainOpenLockedFilesystem(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.detached_descriptors == 1 && details.data_file_states == 1 &&
         details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static struct flock MakeLock(short type, off_t start, off_t length) {
  return (struct flock){
    .l_type = type,
    .l_whence = SEEK_SET,
    .l_start = start,
    .l_len = length,
    .l_pid = -1,
  };
}

static int CheckRecordLocks(int fd) {
  struct flock lock = MakeLock(F_WRLCK, 0, 4);
  if (fcntl(fd, F_SETLK, &lock) != 0 ||
      wasmfs_record_lock_count_for_testing(fd) != 1) {
    return ErrorOrEIO();
  }

  struct flock query = MakeLock(F_WRLCK, 0, 1);
  if (fcntl(fd, F_GETLK, &query) != 0 || query.l_type != F_UNLCK ||
      query.l_whence != SEEK_SET || query.l_start != 0 || query.l_len != 1 ||
      query.l_pid != -1) {
    return ErrorOrEIO();
  }

  // An adjacent to-EOF F_SETLKW is immediately satisfiable under the
  // exclusive profile lease and merges with the finite range above.
  lock = MakeLock(F_WRLCK, 4, 0);
  if (fcntl(fd, F_SETLKW, &lock) != 0 ||
      wasmfs_record_lock_count_for_testing(fd) != 1) {
    return ErrorOrEIO();
  }

  lock = MakeLock(F_UNLCK, 0, 0);
  if (fcntl(fd, F_SETLK, &lock) != 0 ||
      wasmfs_record_lock_count_for_testing(fd) != 0) {
    return ErrorOrEIO();
  }

  lock = MakeLock(F_WRLCK, 1, 2);
  if (fcntl(fd, F_SETLK, &lock) != 0 ||
      wasmfs_record_lock_count_for_testing(fd) != 1) {
    return ErrorOrEIO();
  }
  const int alias = dup(fd);
  if (alias < 0) {
    return ErrorOrEIO();
  }
  if (close(alias) != 0 || wasmfs_record_lock_count_for_testing(fd) != 0) {
    return ErrorOrEIO();
  }

  // A separate open() must resolve to the same logical inode and therefore
  // release this process-owned lock on close, just like POSIX's dcache-backed
  // V4 file does. dup() alone would not prove that identity property.
  lock = MakeLock(F_WRLCK, 1, 2);
  if (fcntl(fd, F_SETLK, &lock) != 0 ||
      wasmfs_record_lock_count_for_testing(fd) != 1) {
    return ErrorOrEIO();
  }
  const int independently_opened = open(kDataPath, O_RDWR);
  if (independently_opened < 0) {
    return ErrorOrEIO();
  }
  if (close(independently_opened) != 0 ||
      wasmfs_record_lock_count_for_testing(fd) != 0) {
    return ErrorOrEIO();
  }

  const int readonly = open(kDataPath, O_RDONLY);
  if (readonly < 0) {
    return ErrorOrEIO();
  }
  lock = MakeLock(F_WRLCK, 0, 1);
  errno = 0;
  const int readonly_result = fcntl(readonly, F_SETLK, &lock);
  const int readonly_error = errno;
  if (close(readonly) != 0) {
    return ErrorOrEIO();
  }
  return readonly_result == -1 && readonly_error == EBADF ? 0 : EIO;
}

struct FcntlCloseRace {
  int fd;
  int result;
  int error;
};

static void* SetRecordLockInThread(void* argument) {
  struct FcntlCloseRace* race = argument;
  struct flock lock = MakeLock(F_WRLCK, 0, 1);
  errno = 0;
  race->result = fcntl(race->fd, F_SETLK, &lock);
  race->error = errno;
  return NULL;
}

static int CheckCloseDuringRecordLock(int* fd) {
  if (!fd || *fd < 0) {
    return EINVAL;
  }
  const int alias = dup(*fd);
  if (alias < 0) {
    return ErrorOrEIO();
  }
  int expected = 0;
  if (!atomic_compare_exchange_strong(&fcntlCloseRaceState, &expected, 1)) {
    close(alias);
    return EBUSY;
  }

  struct FcntlCloseRace race = {
    .fd = *fd,
    .result = 0,
    .error = 0,
  };
  pthread_t thread;
  const int create_error = pthread_create(&thread, NULL, SetRecordLockInThread,
                                          &race);
  if (create_error != 0) {
    atomic_store(&fcntlCloseRaceState, 0);
    close(alias);
    return create_error;
  }

  int reached_recheck = 0;
  for (int attempt = 0; attempt < 10000; ++attempt) {
    if (atomic_load(&fcntlCloseRaceState) == 2) {
      reached_recheck = 1;
      break;
    }
    emscripten_thread_sleep(1);
  }
  if (!reached_recheck) {
    // The test seam failed before its pause point. Abort this isolated module
    // rather than joining a thread that might be wedged before the hook; the
    // browser fixture's event watchdog then reports a bounded test failure.
    abort();
  }

  if (close(*fd) != 0) {
    atomic_store(&fcntlCloseRaceState, 3);
    pthread_join(thread, NULL);
    close(alias);
    return ErrorOrEIO();
  }
  // Reinstall the same OpenFileState rather than merely reopening the same
  // dcache File. The table incarnation must still reject the stale fcntl
  // because the intervening close released every process-owned lock.
  const int reinstalled = dup2(alias, *fd);
  if (reinstalled != *fd) {
    const int dup_error = ErrorOrEIO();
    atomic_store(&fcntlCloseRaceState, 3);
    pthread_join(thread, NULL);
    if (reinstalled >= 0) {
      close(reinstalled);
    }
    close(alias);
    return dup_error;
  }
  atomic_store(&fcntlCloseRaceState, 3);
  if (pthread_join(thread, NULL) != 0) {
    close(reinstalled);
    close(alias);
    return EIO;
  }
  if (race.result != -1 || race.error != EBADF ||
      wasmfs_record_lock_count_for_testing(alias) != 0 ||
      wasmfs_record_lock_count_for_testing(reinstalled) != 0) {
    close(reinstalled);
    close(alias);
    return EIO;
  }
  if (close(alias) != 0) {
    close(reinstalled);
    return ErrorOrEIO();
  }
  *fd = reinstalled;
  return 0;
}

static int OpenAndVerifyData(int create, int* fd_out) {
  if (!fd_out) {
    return EINVAL;
  }
  const int flags = create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR;
  const int fd = open(kDataPath, flags, 0600);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  int error = 0;
  if (create) {
    if (pwrite(fd, kContents, sizeof(kContents), 0) !=
        (ssize_t)sizeof(kContents)) {
      error = ErrorOrEIO();
    }
    if (!error && fdatasync(fd) != 0) {
      error = ErrorOrEIO();
    }
  } else {
    uint8_t actual[sizeof(kContents)] = {};
    if (pread(fd, actual, sizeof(actual), 0) != (ssize_t)sizeof(actual)) {
      error = ErrorOrEIO();
    }
    if (!error && memcmp(actual, kContents, sizeof(actual)) != 0) {
      error = EIO;
    }
  }
  if (error) {
    close(fd);
    return error;
  }
  *fd_out = fd;
  return 0;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int fd = -1;
  int error = 0;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_HOLDER)
  error = MountFilesystem(&backend);
  if (!error) {
    error = OpenAndVerifyData(1, &fd);
  }
  if (!error) {
    error = CheckRecordLocks(fd);
  }
  if (!error) {
    error = CheckCloseDuringRecordLock(&fd);
  }
  if (!error) {
    struct flock lock = MakeLock(F_WRLCK, 0, 0);
    if (fcntl(fd, F_SETLK, &lock) != 0 ||
        wasmfs_record_lock_count_for_testing(fd) != 1) {
      error = ErrorOrEIO();
    }
  }
  if (error) {
    if (fd >= 0) {
      close(fd);
    }
    if (backend) {
      const int drain_error = DrainFilesystem(backend);
      if (!error) {
        error = drain_error;
      }
    }
    Report(kHolder, error, 0);
  } else {
    // Keep the fcntl lock and the Web Lock alive until the parent disposes
    // this iframe. A contender must fail before it can construct another V4
    // backend, so there is no second process with which F_SETLKW could block.
    Report(kHolder, 0, 1);
  }
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_CONTENDER)
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  error = !backend && errno == EBUSY ? 0 : EIO;
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(kContender, error, 0);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_DRAINER)
  error = MountFilesystem(&backend);
  if (!error) {
    error = OpenAndVerifyData(0, &fd);
  }
  if (!error) {
    error = CheckRecordLocks(fd);
  }
  if (!error) {
    struct flock lock = MakeLock(F_WRLCK, 0, 0);
    if (fcntl(fd, F_SETLK, &lock) != 0 ||
        wasmfs_record_lock_count_for_testing(fd) != 1) {
      error = ErrorOrEIO();
    }
  }
  if (!error) {
    error = DrainOpenLockedFilesystem(backend);
    if (!error) {
      errno = 0;
      if (fcntl(fd, F_GETFL) != -1 || errno != EBADF) {
        error = ErrorOrEIO();
      }
    }
    backend = NULL;
    // The backend drain detached and closed this descriptor. Do not call
    // close() on its now-invalid number: a future descriptor could reuse it.
    fd = -1;
  }
  if (fd >= 0) {
    close(fd);
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(kDrainer, error, !error);
#else
  error = MountFilesystem(&backend);
  if (!error) {
    error = OpenAndVerifyData(0, &fd);
  }
  if (!error) {
    error = CheckRecordLocks(fd);
  }
  if (fd >= 0 && close(fd) != 0 && !error) {
    error = ErrorOrEIO();
  }
  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(kVerifier, error, 0);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
