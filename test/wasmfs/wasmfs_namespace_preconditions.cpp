/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/virtual.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

constexpr char DetachedMountPath[] = "/wasmfs-detached-parent";
constexpr char ReadOnlyParentPath[] = "/wasmfs-read-only-parent";
constexpr char MountedSourcePath[] = "/wasmfs-mounted-rename-source";
constexpr char MountedSourceMovedPath[] =
  "/wasmfs-mounted-rename-source-moved";
constexpr char MountedDestinationPath[] =
  "/wasmfs-mounted-rename-destination";
constexpr char RegularSourcePath[] = "/wasmfs-regular-rename-source";
constexpr char SymlinkFailureMountPath[] = "/wasmfs-symlink-failure";

class SymlinkFailureDirectory : public wasmfs::MemoryDirectory {
  int& insertSymlinkCalls;

protected:
  std::shared_ptr<wasmfs::Symlink>
  insertSymlink(const std::string&, const std::string&) override {
    ++insertSymlinkCalls;
    // Deliberately leave MemoryDirectory::entries untouched. This models the
    // pointer-only insertion failure contract without an ambiguous side
    // effect in the test double.
    return nullptr;
  }

public:
  SymlinkFailureDirectory(mode_t mode,
                          wasmfs::backend_t backend,
                          int& insertSymlinkCalls)
    : MemoryDirectory(mode, backend), insertSymlinkCalls(insertSymlinkCalls) {}
};

class SymlinkFailureBackend : public wasmfs::Backend {
public:
  int insertSymlinkCalls = 0;
  SymlinkFailureDirectory* directory = nullptr;

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDataFile>(mode, this);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto result = std::make_shared<SymlinkFailureDirectory>(
      mode, this, insertSymlinkCalls);
    directory = result.get();
    return result;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string) override {
    return nullptr;
  }
};

void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void assertMissing(const char* path) {
  errno = 0;
  assert(access(path, F_OK) == -1);
  assert(errno == ENOENT);
}

void createRegularFile(const char* path) {
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);
}

// VirtualDirectory must hand its real directory a real child. In particular,
// this exercises the symlink branch of devirtualize(), which is otherwise
// easy to miss because the case-insensitive directory overrides insertMove.
void testVirtualSymlinkMove() {
  auto realBackend = wasmfs::createMemoryBackend();
  auto virtualBackend = wasmfs::createMemoryBackend();
  auto realRoot = realBackend->createDirectory(0700);
  auto virtualRoot =
    std::make_shared<wasmfs::VirtualDirectory>(realRoot, virtualBackend);
  realRoot->locked().setParent(realRoot);
  virtualRoot->locked().setParent(virtualRoot);

  auto realLink = realRoot->locked().insertSymlink("old", "target");
  assert(realLink);
  auto virtualLink =
    std::make_shared<wasmfs::VirtualSymlink>(realLink, virtualBackend);
  assert(virtualRoot->locked().mountChild("old", virtualLink));

  assert(virtualRoot->locked().insertMove("new", virtualLink) == 0);
  assert(virtualRoot->locked().getChild("old") == nullptr);
  assert(virtualRoot->locked().getChild("new") == virtualLink);
  assert(realRoot->locked().getChild("old") == nullptr);
  assert(realRoot->locked().getChild("new") == realLink);
  assert(virtualLink->locked().getParent() == virtualRoot);
  assert(realLink->locked().getParent() == realRoot);
}

void testDetachedParentCannotCreate() {
  ::backend_t backend = wasmfs_create_memory_backend();
  assert(backend);
  assert(wasmfs_create_directory(DetachedMountPath, 0555, backend) == 0);

  auto root = wasmfs::wasmFS.getRootDirectory();
  auto mountedFile = root->locked().getChild("wasmfs-detached-parent");
  assert(mountedFile);
  auto mounted = mountedFile->cast<wasmfs::Directory>();
  assert(mounted->locked().getParent() == root);

  int dirfd = open(DetachedMountPath, O_RDONLY | O_DIRECTORY);
  assert(dirfd >= 0);
  assert(wasmfs_unmount(DetachedMountPath) == 0);

  // Detaching a cache-only mount must also make retained references observe
  // that it has no linked parent.
  assert(!mounted->locked().getParent());

  // Linkage takes precedence over write permission: this retained directory
  // is deliberately read-only as well as detached.
  errno = 0;
  expectFailure(mkdirat(dirfd, "directory", 0700), ENOENT);
  errno = 0;
  expectFailure(symlinkat("target", dirfd, "link"), ENOENT);
  assert(close(dirfd) == 0);
}

void testSymlinkCreateRequiresWritableParent() {
  assert(mkdir(ReadOnlyParentPath, 0555) == 0);
  errno = 0;
  expectFailure(symlink("target", "/wasmfs-read-only-parent/link"), EACCES);
  assertMissing("/wasmfs-read-only-parent/link");
  assert(chmod(ReadOnlyParentPath, 0755) == 0);
  assert(rmdir(ReadOnlyParentPath) == 0);
}

void testSymlinkCreateReportsBackendFailure() {
  auto backend = std::make_unique<SymlinkFailureBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  assert(wasmfs_create_directory(
           SymlinkFailureMountPath,
           0700,
           reinterpret_cast<::backend_t>(backendHandle)) == 0);

  errno = 0;
  expectFailure(symlink("target", "/wasmfs-symlink-failure/link"), EIO);
  assert(backendState->insertSymlinkCalls == 1);
  assert(backendState->directory);
  assert(backendState->directory->locked().getNumEntries() == 0);
  assertMissing("/wasmfs-symlink-failure/link");
  assert(wasmfs_unmount(SymlinkFailureMountPath) == 0);
}

void testRenameRejectsMountEntries() {
  ::backend_t backend = wasmfs_create_memory_backend();
  assert(backend);
  assert(wasmfs_create_directory(MountedSourcePath, 0700, backend) == 0);
  assert(wasmfs_create_directory(MountedDestinationPath, 0700, backend) == 0);
  createRegularFile(RegularSourcePath);

  errno = 0;
  expectFailure(rename(MountedSourcePath, MountedSourceMovedPath), EBUSY);
  assert(access(MountedSourcePath, F_OK) == 0);
  assertMissing(MountedSourceMovedPath);

  errno = 0;
  expectFailure(rename(RegularSourcePath, MountedDestinationPath), EBUSY);
  assert(access(RegularSourcePath, F_OK) == 0);
  assert(access(MountedDestinationPath, F_OK) == 0);

  assert(unlink(RegularSourcePath) == 0);
  assert(wasmfs_unmount(MountedSourcePath) == 0);
  assert(wasmfs_unmount(MountedDestinationPath) == 0);
}

} // anonymous namespace

int main() {
  testVirtualSymlinkMove();
  testDetachedParentCannotCreate();
  testSymlinkCreateRequiresWritableParent();
  testSymlinkCreateReportsBackendFailure();
  testRenameRejectsMountEntries();
  puts("ok");
}
