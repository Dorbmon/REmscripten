/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

class FlushDirectory : public wasmfs::MemoryDirectory {
public:
  int flushResult = -EIO;
  int flushCalls = 0;

  FlushDirectory(mode_t mode, wasmfs::backend_t backend)
    : MemoryDirectory(mode, backend) {}

protected:
  int flush() override {
    ++flushCalls;
    return flushResult;
  }
};

class FlushBackend : public wasmfs::Backend {
public:
  FlushDirectory* directory = nullptr;

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDataFile>(mode, this);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto result = std::make_shared<FlushDirectory>(mode, this);
    directory = result.get();
    return result;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(target, this);
  }
};

constexpr char MountPath[] = "/wasmfs-directory-flush";

void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void testDefaultDirectoryFlushFailsExplicitly() {
  int fd = open(".", O_RDONLY);
  assert(fd >= 0);

  errno = 0;
  expectFailure(fsync(fd), ENOTSUP);

  errno = 0;
  expectFailure(fdatasync(fd), ENOTSUP);

  assert(close(fd) == 0);
}

void testDirectoryFlushResultPropagates() {
  auto backend = std::make_unique<FlushBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  ::backend_t publicBackend = reinterpret_cast<::backend_t>(backendHandle);
  assert(wasmfs_create_directory(MountPath, 0700, publicBackend) == 0);
  assert(backendState->directory);

  int fd = open(MountPath, O_RDONLY);
  assert(fd >= 0);

  errno = 0;
  expectFailure(fsync(fd), EIO);
  assert(backendState->directory->flushCalls == 1);

  errno = 0;
  expectFailure(fdatasync(fd), EIO);
  assert(backendState->directory->flushCalls == 2);

  backendState->directory->flushResult = 0;

  errno = 0;
  assert(fsync(fd) == 0);
  assert(errno == 0);
  assert(backendState->directory->flushCalls == 3);

  errno = 0;
  assert(fdatasync(fd) == 0);
  assert(errno == 0);
  assert(backendState->directory->flushCalls == 4);

  assert(close(fd) == 0);
  assert(wasmfs_unmount(MountPath) == 0);
}

} // anonymous namespace

int main() {
  testDefaultDirectoryFlushFailsExplicitly();
  testDirectoryFlushResultPropagates();
  puts("ok");
}
