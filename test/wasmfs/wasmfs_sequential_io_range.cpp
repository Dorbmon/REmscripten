/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <stdio.h>
#include <string>
#include <sys/uio.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

constexpr off_t MaxOffset = static_cast<off_t>(LLONG_MAX);

void expectFailure(ssize_t result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void expectPosition(int fd, off_t expected) {
  errno = 0;
  assert(lseek(fd, 0, SEEK_CUR) == expected);
}

// Linux permits high offsets on /dev/null. Its no-op WasmFS backend makes it
// a harmless pre-fix false-success witness: a core endpoint overflow must
// fail before a backend can return a short read or write success.
void testSequentialReads() {
  int fd = open("/dev/null", O_RDONLY);
  assert(fd >= 0);
  assert(lseek(fd, MaxOffset - 1, SEEK_SET) == MaxOffset - 1);

  char scalar[] = {'s', 'c'};
  errno = 0;
  expectFailure(read(fd, scalar, sizeof(scalar)), EINVAL);
  assert(scalar[0] == 's');
  assert(scalar[1] == 'c');
  expectPosition(fd, MaxOffset - 1);

  char first = 'x';
  char second = 'y';
  iovec iovs[] = {{&first, 1}, {&second, 1}};
  errno = 0;
  expectFailure(readv(fd, iovs, 2), EINVAL);
  assert(first == 'x');
  assert(second == 'y');
  expectPosition(fd, MaxOffset - 1);

  assert(close(fd) == 0);
}

void testSequentialWrites() {
  int fd = open("/dev/null", O_WRONLY);
  assert(fd >= 0);
  assert(lseek(fd, MaxOffset - 1, SEEK_SET) == MaxOffset - 1);

  const char scalar[] = {'s', 'c'};
  errno = 0;
  expectFailure(write(fd, scalar, sizeof(scalar)), EINVAL);
  expectPosition(fd, MaxOffset - 1);

  char first = 'x';
  char second = 'y';
  iovec iovs[] = {{&first, 1}, {&second, 1}};
  errno = 0;
  expectFailure(writev(fd, iovs, 2), EINVAL);
  expectPosition(fd, MaxOffset - 1);

  assert(close(fd) == 0);
}

void testSeekOverflow() {
  const char path[] = "wasmfs-sequential-io-range";
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(write(fd, "x", 1) == 1);
  assert(lseek(fd, 1, SEEK_SET) == 1);

  errno = 0;
  expectFailure(lseek(fd, MaxOffset, SEEK_CUR), EINVAL);
  expectPosition(fd, 1);

  errno = 0;
  expectFailure(lseek(fd, MaxOffset, SEEK_END), EINVAL);
  expectPosition(fd, 1);

  assert(close(fd) == 0);
  assert(unlink(path) == 0);
}

class HugeSizeFile : public wasmfs::DataFile {
  int& writeCalls;

  int open(wasmfs::oflags_t) override { return 0; }
  int close() override { return 0; }

  ssize_t read(uint8_t*, size_t, off_t) override { return 0; }

  ssize_t write(const uint8_t*, size_t len, off_t) override {
    ++writeCalls;
    return len;
  }

  int flush() override { return 0; }
  off_t getSize() override { return MaxOffset - 1; }
  int setSize(off_t) override { return 0; }

public:
  HugeSizeFile(mode_t mode,
               wasmfs::backend_t backend,
               int& writeCalls)
    : DataFile(mode, backend), writeCalls(writeCalls) {}
};

class HugeSizeBackend : public wasmfs::Backend {
public:
  int writeCalls = 0;

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<HugeSizeFile>(mode, this, writeCalls);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDirectory>(mode, this);
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(target, this);
  }
};

void testAppendRange() {
  auto backend = std::make_unique<HugeSizeBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  const char directory[] = "/wasmfs-sequential-io-range-append";
  // The public mount API carries an opaque backend pointer.
  ::backend_t publicBackend = reinterpret_cast<::backend_t>(backendHandle);
  assert(wasmfs_create_directory(directory, 0700, publicBackend) == 0);

  const char path[] = "/wasmfs-sequential-io-range-append/file";
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_APPEND, 0600);
  assert(fd >= 0);
  expectPosition(fd, 0);

  const char scalar[] = {'s', 'c'};
  errno = 0;
  expectFailure(write(fd, scalar, sizeof(scalar)), EINVAL);
  assert(backendState->writeCalls == 0);
  expectPosition(fd, 0);

  char first = 'x';
  char second = 'y';
  iovec iovs[] = {{&first, 1}, {&second, 1}};
  errno = 0;
  expectFailure(writev(fd, iovs, 2), EINVAL);
  assert(backendState->writeCalls == 0);
  expectPosition(fd, 0);

  assert(close(fd) == 0);
  assert(unlink(path) == 0);
  assert(wasmfs_unmount(directory) == 0);
}

} // anonymous namespace

int main() {
  testSequentialReads();
  testSequentialWrites();
  testSeekOverflow();
  testAppendRange();
  puts("ok");
  return 0;
}
