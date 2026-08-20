/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace wasmfs {

// Use the real WasmFS root creation hook so this test reaches the otherwise
// skipped CWD == root branch in __syscall_getcwd().
class RootAdmissionBackend : public Backend {
public:
  bool sealed = false;

  int acquireProfileOperation() override {
    return sealed ? -ESHUTDOWN : 0;
  }

  std::shared_ptr<DataFile> createFile(mode_t mode) override {
    return std::make_shared<MemoryDataFile>(mode, this);
  }

  std::shared_ptr<Directory> createDirectory(mode_t mode) override {
    return std::make_shared<MemoryDirectory>(mode, this);
  }

  std::shared_ptr<Symlink> createSymlink(std::string target) override {
    return std::make_shared<MemorySymlink>(std::move(target), this);
  }
};

static RootAdmissionBackend* rootAdmissionBackend = nullptr;

} // namespace wasmfs

extern "C" ::backend_t wasmfs_create_root_dir(void) {
  auto backend = std::make_unique<wasmfs::RootAdmissionBackend>();
  wasmfs::rootAdmissionBackend = backend.get();
  return reinterpret_cast<::backend_t>(
    wasmfs::wasmFS.addBackend(std::move(backend)));
}

namespace {

constexpr char FirstMount[] = "/wasmfs-profile-drain-first";
constexpr char FirstFile[] = "/wasmfs-profile-drain-first/file";
constexpr char SecondMount[] = "/wasmfs-profile-drain-second";
constexpr char SecondFile[] = "/wasmfs-profile-drain-second/file";

class TrackingBackend : public wasmfs::Backend {
public:
  int acquireCalls = 0;
  int releaseCalls = 0;

  int acquireProfileOperation() override {
    ++acquireCalls;
    return 0;
  }

  void releaseProfileOperation() override { ++releaseCalls; }

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDataFile>(mode, this);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDirectory>(mode, this);
  }

  std::shared_ptr<wasmfs::Symlink>
  createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(std::move(target), this);
  }
};

void testRootCwdAdmission() {
  auto* rootBackend = wasmfs::rootAdmissionBackend;
  assert(rootBackend);
  // The actual global root and CWD must belong to the backend whose admission
  // we control; this makes getcwd() take the real CWD == root syscall path.
  assert(wasmfs::wasmFS.getRootDirectory()->getBackend() == rootBackend);
  assert(wasmfs::wasmFS.getCWD()->getBackend() == rootBackend);

  rootBackend->sealed = true;
  char cwd[PATH_MAX];
  errno = 0;
  assert(getcwd(cwd, sizeof(cwd)) == nullptr);
  assert(errno == ESHUTDOWN);
  rootBackend->sealed = false;
}

void testOuterOperationToken(wasmfs::backend_t backend,
                             TrackingBackend* tracking) {
  const int acquiresBefore = tracking->acquireCalls;
  const int releasesBefore = tracking->releaseCalls;
  {
    wasmfs::WasmFS::Operation operation(wasmfs::wasmFS);
    assert(operation);
    assert(operation.admitBackend(backend) == 0);
    // An outer syscall can find the same backend through multiple path/fd
    // steps. It owns exactly one token until the full operation returns.
    assert(operation.admitBackend(backend) == 0);
    assert(tracking->acquireCalls == acquiresBefore + 1);
    assert(tracking->releaseCalls == releasesBefore);
  }
  assert(tracking->releaseCalls == releasesBefore + 1);
}

void testFilteredDetach(wasmfs::backend_t first,
                        wasmfs::backend_t second) {
  const auto publicFirst = reinterpret_cast<::backend_t>(first);
  const auto publicSecond = reinterpret_cast<::backend_t>(second);
  assert(wasmfs_create_directory(FirstMount, 0700, publicFirst) == 0);
  assert(wasmfs_create_directory(SecondMount, 0700, publicSecond) == 0);

  int firstFD = open(FirstFile, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(firstFD >= 0);
  assert(write(firstFD, "a", 1) == 1);
  int firstAlias = dup(firstFD);
  assert(firstAlias >= 0);
  int firstDirectory = open(FirstMount, O_RDONLY | O_DIRECTORY);
  assert(firstDirectory >= 0);

  int secondFD = open(SecondFile, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(secondFD >= 0);

  uint32_t detached = 0;
  std::vector<std::shared_ptr<wasmfs::DataFile>> closees;
  {
    auto table = wasmfs::wasmFS.getFileTable().locked();
    closees = table.detachBackend(first, detached);
  }

  // Every first-backend fd slot is removed, but aliases produce one final
  // open-file state for later close outside the FileTable lock.
  assert(detached == 3);
  assert(closees.size() == 1);
  for (auto& closee : closees) {
    auto file = closee->locked();
    assert(file.flush() == 0);
    assert(file.close() == 0);
  }

  struct stat statBuffer;
  errno = 0;
  assert(fstat(firstFD, &statBuffer) == -1);
  assert(errno == EBADF);
  errno = 0;
  assert(fcntl(firstAlias, F_GETFL) == -1);
  assert(errno == EBADF);
  errno = 0;
  assert(fstat(firstDirectory, &statBuffer) == -1);
  assert(errno == EBADF);

  // An unrelated backend and its descriptor stay usable after a filtered
  // detach. This is the nonbrowser counterpart to the leased-OPFS browser
  // handoff test, which exercises sealing and the real Web Lock.
  assert(write(secondFD, "b", 1) == 1);
  assert(close(secondFD) == 0);
}

} // namespace

int main() {
  testRootCwdAdmission();

  auto firstBackend = std::make_unique<TrackingBackend>();
  auto* firstTracking = firstBackend.get();
  auto first = wasmfs::wasmFS.addBackend(std::move(firstBackend));
  auto second = wasmfs::wasmFS.addBackend(std::make_unique<TrackingBackend>());

  testOuterOperationToken(first, firstTracking);
  testFilteredDetach(first, second);
  puts("success");
  return 0;
}
