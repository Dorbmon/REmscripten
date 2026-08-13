/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <wasi/api.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

constexpr char MountPath[] = "/wasmfs-terminal-drain";
constexpr char FilePath[] = "/wasmfs-terminal-drain/file";

class TrackingDataFile : public wasmfs::MemoryDataFile {
public:
  bool failFlush = false;
  int openCalls = 0;
  int flushCalls = 0;
  int closeCalls = 0;

  TrackingDataFile(mode_t mode, wasmfs::backend_t backend)
    : MemoryDataFile(mode, backend) {}

protected:
  int open(wasmfs::oflags_t flags) override {
    ++openCalls;
    return 0;
  }

  int flush() override {
    ++flushCalls;
    if (failFlush) {
      return -EIO;
    }
    return 0;
  }

  int close() override {
    ++closeCalls;
    return 0;
  }
};

class TrackingBackend : public wasmfs::Backend {
public:
  TrackingDataFile* dataFile = nullptr;

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    auto file = std::make_shared<TrackingDataFile>(mode, this);
    dataFile = file.get();
    return file;
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDirectory>(mode, this);
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(target, this);
  }
};

std::atomic<int> operationEntered = 0;
std::atomic<int> operationRelease = 0;
std::atomic<int> drainCalling = 0;
std::atomic<int> drainReturned = 0;
std::atomic<int> holderAboutToRelease = 0;

void* holdOperation(void*) {
  {
    wasmfs::WasmFS::Operation operation(wasmfs::wasmFS);
    assert(operation);
    operationEntered.store(1);
    while (!operationRelease.load()) {
      emscripten_thread_sleep(1);
    }
    // terminalDrain cannot return until this Operation destructor has reduced
    // the active-operation count. Mark immediately before that destruction so
    // the witness has no scheduler race after the destructor runs.
    holderAboutToRelease.store(1);
  }
  return nullptr;
}

void* releaseOperation(void*) {
  while (!operationEntered.load()) {
    emscripten_thread_sleep(1);
  }
  while (!drainCalling.load()) {
    emscripten_thread_sleep(1);
  }
  emscripten_thread_sleep(20);
  assert(!drainReturned.load());
  operationRelease.store(1);
  return nullptr;
}

void checkPostTerminalAdmission() {
  errno = 0;
  assert(open(FilePath, O_RDONLY) == -1);
  assert(errno == ESHUTDOWN);

  errno = 0;
  assert(wasmfs_create_memory_backend() == nullptr);
  assert(errno == ESHUTDOWN);

  const char byte = 'x';
  __wasi_ciovec_t iov;
  iov.buf = reinterpret_cast<uint8_t*>(const_cast<char*>(&byte));
  iov.buf_len = 1;
  __wasi_size_t written = 0;
  assert(__wasi_fd_write(1, &iov, 1, &written) == __WASI_ERRNO_CANCELED);

  char readByte = 0;
  assert(read(0, &readByte, 1) == -1);
  assert(errno == ECANCELED);

  errno = 0;
  wasmfs_flush();
  assert(errno == ESHUTDOWN);

  wasmfs_terminal_drain_result again = {};
  assert(wasmfs_terminal_drain(&again) == -ESHUTDOWN);

  void* anonymous = mmap(NULL,
                         4096,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);
  assert(anonymous != MAP_FAILED);
  assert(munmap(anonymous, 4096) == 0);
}

#ifdef WASMFS_TERMINAL_DRAIN_COOKIE_REENTRY

struct CookieState {
  int callbackCalls = 0;
  int callbackErrno = 0;
};

ssize_t cookieWrite(void* cookie, const char*, size_t) {
  auto* state = static_cast<CookieState*>(cookie);
  ++state->callbackCalls;
  errno = 0;
  assert(open(FilePath, O_RDONLY) == -1);
  state->callbackErrno = errno;
  return -1;
}

void runCookieReentryTest() {
  CookieState state;
  // stdout is line-buffered. Do not include a newline: terminalDrain's
  // fflush(NULL) must take the narrow __wasi_fd_write admission path to emit
  // this marker.
  assert(fputs("success", stdout) >= 0);
  cookie_io_functions_t callbacks = {};
  callbacks.write = cookieWrite;
  FILE* cookie = fopencookie(&state, "w", callbacks);
  assert(cookie);
  assert(fputs("cookie", cookie) >= 0);

  wasmfs_terminal_drain_result result = {};
  assert(wasmfs_terminal_drain(&result) == -ESHUTDOWN);
  assert(result.error == -ESHUTDOWN);
  assert(result.libc_flush_failed == 1);
  assert(state.callbackCalls == 1);
  assert(state.callbackErrno == ESHUTDOWN);
  assert(fclose(cookie) == 0);
  checkPostTerminalAdmission();
}

#else

void runDrainTest() {
  auto backend = std::make_unique<TrackingBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  auto publicBackend = reinterpret_cast<::backend_t>(backendHandle);
  assert(wasmfs_create_directory(MountPath, 0700, publicBackend) == 0);

  int first = open(FilePath, O_CREAT | O_RDWR, 0600);
  assert(first >= 0);
  int alias = dup(first);
  assert(alias >= 0);
  int second = open(FilePath, O_RDWR);
  assert(second >= 0);
  assert(backendState->dataFile);
  assert(backendState->dataFile->openCalls == 2);

  // A drain issued from an admitted operation must fail before changing the
  // terminal state; after this scope, the normal drain below still succeeds.
  {
    wasmfs::WasmFS::Operation operation(wasmfs::wasmFS);
    assert(operation);
    wasmfs_terminal_drain_result selfDrain = {};
    assert(wasmfs_terminal_drain(&selfDrain) == -EDEADLK);
  }

  // __syscall_stat64 delegates to __syscall_newfstatat. This exercises
  // reentrant Operation depth accounting before the drain waits for zero.
  struct stat statBuffer;
  assert(stat(FilePath, &statBuffer) == 0);

  // Retain a file-backed mapping through the terminal transition. The outer
  // mmap registry fence must reject its later msync/munmap while fresh
  // anonymous mappings remain independent of WasmFS.
  void* fileMapping = mmap(NULL, 1, PROT_READ, MAP_PRIVATE, first, 0);
  assert(fileMapping != MAP_FAILED);

  // This must enter __wasi_fd_write while terminalDrain's fflush(NULL) is
  // active. Completion without a deadlock exercises the narrow stdio path.
  assert(fputs("success", stdout) >= 0);

  pthread_t holder;
  pthread_t releaser;
  assert(pthread_create(&holder, nullptr, holdOperation, nullptr) == 0);
  assert(pthread_create(&releaser, nullptr, releaseOperation, nullptr) == 0);
  while (!operationEntered.load()) {
    emscripten_thread_sleep(1);
  }

#ifdef WASMFS_TERMINAL_DRAIN_FLUSH_FAILURE
  backendState->dataFile->failFlush = true;
#endif

  wasmfs_terminal_drain_result result = {};
  drainCalling.store(1);
  int drainResult = wasmfs_terminal_drain(&result);
  drainReturned.store(1);
  assert(pthread_join(holder, nullptr) == 0);
  assert(pthread_join(releaser, nullptr) == 0);
  assert(holderAboutToRelease.load());

  // stdin, stdout, stderr, then the two independent FilePath open states.
  // dup(first) is an alias of the first state and must not add another close.
  assert(result.data_file_states == 5);
  assert(backendState->dataFile->flushCalls == 2);
  assert(backendState->dataFile->closeCalls == 2);

#ifdef WASMFS_TERMINAL_DRAIN_FLUSH_FAILURE
  assert(drainResult == -EIO);
  assert(result.error == -EIO);
  assert(result.data_flush_failures == 2);
#else
  assert(drainResult == 0);
  assert(result.error == 0);
  assert(result.data_flush_failures == 0);
#endif
  assert(result.libc_flush_failed == 0);
  assert(result.data_close_failures == 0);

  errno = 0;
  assert(msync(fileMapping, 1, MS_SYNC) == -1);
  assert(errno == ESHUTDOWN);
  errno = 0;
  assert(munmap(fileMapping, 1) == -1);
  assert(errno == ESHUTDOWN);
  checkPostTerminalAdmission();
}

#endif

} // anonymous namespace

int main() {
  // The test command uses PROXY_TO_PTHREAD, so main is not Emscripten's
  // runtime thread and a terminal drain is allowed.
  assert(!emscripten_is_main_runtime_thread());
#ifdef WASMFS_TERMINAL_DRAIN_COOKIE_REENTRY
  runCookieReentryTest();
#else
  runDrainTest();
#endif
  return 0;
}
