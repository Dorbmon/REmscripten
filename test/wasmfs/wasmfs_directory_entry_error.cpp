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
#include <unistd.h>
#include <utility>
#include <vector>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

class ErrorDirectory : public wasmfs::MemoryDirectory {
public:
  bool failEntryCount = false;
  int entryCountCalls = 0;
  int removeChildCalls = 0;
  int insertMoveCalls = 0;

  ErrorDirectory(mode_t mode, wasmfs::backend_t backend)
    : MemoryDirectory(mode, backend) {}

  void resetMutationCounts() {
    removeChildCalls = 0;
    insertMoveCalls = 0;
  }

protected:
  ssize_t getNumEntries() override {
    ++entryCountCalls;
    if (failEntryCount) {
      return -EIO;
    }
    return MemoryDirectory::getNumEntries();
  }

  int removeChild(const std::string& name) override {
    ++removeChildCalls;
    return MemoryDirectory::removeChild(name);
  }

  int insertMove(const std::string& name,
                 std::shared_ptr<wasmfs::File> file) override {
    ++insertMoveCalls;
    return MemoryDirectory::insertMove(name, std::move(file));
  }
};

class ErrorBackend : public wasmfs::Backend {
public:
  std::vector<ErrorDirectory*> directories;

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDataFile>(mode, this);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto directory = std::make_shared<ErrorDirectory>(mode, this);
    directories.push_back(directory.get());
    return directory;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(target, this);
  }
};

constexpr char MountPath[] = "/wasmfs-directory-entry-error";
constexpr char RmdirPath[] = "/wasmfs-directory-entry-error/rmdir";
constexpr char RmdirChildPath[] =
  "/wasmfs-directory-entry-error/rmdir/child";
constexpr char RenameSourcePath[] = "/wasmfs-directory-entry-error/source";
constexpr char RenameSourceChildPath[] =
  "/wasmfs-directory-entry-error/source/child";
constexpr char RenameDestinationPath[] =
  "/wasmfs-directory-entry-error/destination";
constexpr char RenameDestinationChildPath[] =
  "/wasmfs-directory-entry-error/destination/child";

void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void createFile(const char* path) {
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);
}

void assertExists(const char* path) {
  struct stat statBuf;
  assert(stat(path, &statBuf) == 0);
}

void testDirectoryEntryErrors() {
  auto backend = std::make_unique<ErrorBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  ::backend_t publicBackend = reinterpret_cast<::backend_t>(backendHandle);
  assert(wasmfs_create_directory(MountPath, 0700, publicBackend) == 0);
  assert(backendState->directories.size() == 1);
  auto* mount = backendState->directories.back();

  assert(mkdir(RmdirPath, 0700) == 0);
  auto* rmdirTarget = backendState->directories.back();
  createFile(RmdirChildPath);

  rmdirTarget->failEntryCount = true;
  errno = 0;
  expectFailure(rmdir(RmdirPath), EIO);
  assert(rmdirTarget->entryCountCalls == 1);
  assert(mount->removeChildCalls == 0);
  assert(mount->insertMoveCalls == 0);
  assertExists(RmdirPath);
  assertExists(RmdirChildPath);
  rmdirTarget->failEntryCount = false;

  assert(mkdir(RenameSourcePath, 0700) == 0);
  createFile(RenameSourceChildPath);
  assert(mkdir(RenameDestinationPath, 0700) == 0);
  auto* renameDestination = backendState->directories.back();
  createFile(RenameDestinationChildPath);

  mount->resetMutationCounts();
  renameDestination->failEntryCount = true;
  errno = 0;
  expectFailure(rename(RenameSourcePath, RenameDestinationPath), EIO);
  assert(renameDestination->entryCountCalls == 1);
  assert(mount->removeChildCalls == 0);
  assert(mount->insertMoveCalls == 0);
  assertExists(RenameSourcePath);
  assertExists(RenameSourceChildPath);
  assertExists(RenameDestinationPath);
  assertExists(RenameDestinationChildPath);
  renameDestination->failEntryCount = false;

  assert(unlink(RenameSourceChildPath) == 0);
  assert(rmdir(RenameSourcePath) == 0);
  assert(unlink(RenameDestinationChildPath) == 0);
  assert(rmdir(RenameDestinationPath) == 0);
  assert(unlink(RmdirChildPath) == 0);
  assert(rmdir(RmdirPath) == 0);
  assert(wasmfs_unmount(MountPath) == 0);
}

} // anonymous namespace

int main() {
  testDirectoryEntryErrors();
  puts("ok");
  return 0;
}
