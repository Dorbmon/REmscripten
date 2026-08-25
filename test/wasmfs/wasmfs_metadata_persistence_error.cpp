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
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

struct MetadataPersistenceState {
  int result = 0;
  int calls = 0;
  wasmfs::File::Metadata visibleAtPersist = {};
  wasmfs::File::Metadata candidate = {};
};

class MetadataPersistenceDataFile : public wasmfs::MemoryDataFile {
  MetadataPersistenceState& state;

protected:
  int persistMetadata(const Metadata& metadata) override {
    ++state.calls;
    // File::Handle must not publish the candidate before the backend accepts
    // it. Capture the live image from inside the persistence hook to make
    // that ordering observable in this focused unit.
    state.visibleAtPersist = {mode, atime, mtime, ctime};
    state.candidate = metadata;
    return state.result;
  }

public:
  MetadataPersistenceDataFile(mode_t mode,
                              wasmfs::backend_t backend,
                              MetadataPersistenceState& state)
    : MemoryDataFile(mode, backend), state(state) {}
};

class MetadataPersistenceDirectory : public wasmfs::MemoryDirectory {
  MetadataPersistenceState& state;

protected:
  int persistMetadata(const Metadata& metadata) override {
    ++state.calls;
    state.visibleAtPersist = {mode, atime, mtime, ctime};
    state.candidate = metadata;
    return state.result;
  }

public:
  MetadataPersistenceDirectory(mode_t mode,
                               wasmfs::backend_t backend,
                               MetadataPersistenceState& state)
    : MemoryDirectory(mode, backend), state(state) {}
};

class MetadataPersistenceBackend : public wasmfs::Backend {
public:
  MetadataPersistenceState state;
  MetadataPersistenceDataFile* file = nullptr;
  MetadataPersistenceDirectory* directory = nullptr;

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    auto result = std::make_shared<MetadataPersistenceDataFile>(
      mode, this, state);
    file = result.get();
    return result;
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto result = std::make_shared<MetadataPersistenceDirectory>(
      mode, this, state);
    directory = result.get();
    return result;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(std::move(target), this);
  }
};

constexpr char MountPath[] = "/wasmfs-metadata-persistence-error";
constexpr char FilePath[] = "/wasmfs-metadata-persistence-error/file";

void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void assertSameMetadata(const wasmfs::File::Metadata& actual,
                        const wasmfs::File::Metadata& expected) {
  assert(actual.mode == expected.mode);
  assert(actual.atime == expected.atime);
  assert(actual.mtime == expected.mtime);
  assert(actual.ctime == expected.ctime);
}

void assertSameStatMetadata(const struct stat& actual,
                            const struct stat& expected) {
  assert(actual.st_mode == expected.st_mode);
  assert(actual.st_atim.tv_sec == expected.st_atim.tv_sec);
  assert(actual.st_atim.tv_nsec == expected.st_atim.tv_nsec);
  assert(actual.st_mtim.tv_sec == expected.st_mtim.tv_sec);
  assert(actual.st_mtim.tv_nsec == expected.st_mtim.tv_nsec);
  assert(actual.st_ctim.tv_sec == expected.st_ctim.tv_sec);
  assert(actual.st_ctim.tv_nsec == expected.st_ctim.tv_nsec);
}

wasmfs::File::Metadata getMetadata(wasmfs::File* file) {
  return file->locked().getMetadata();
}

void assertUnchanged(int fd,
                     wasmfs::File* file,
                     const wasmfs::File::Metadata& expected,
                     const struct stat& expectedStat) {
  struct stat statBuffer;
  assert(fstat(fd, &statBuffer) == 0);
  assertSameMetadata(getMetadata(file), expected);
  assertSameStatMetadata(statBuffer, expectedStat);
}

void assertRejectedBeforePublish(MetadataPersistenceBackend* backend,
                                 int fd,
                                 wasmfs::File* file,
                                 const wasmfs::File::Metadata& before,
                                 const struct stat& beforeStat,
                                 int callsBefore) {
  assert(backend->state.calls == callsBefore + 1);
  assertSameMetadata(backend->state.visibleAtPersist, before);
  assertUnchanged(fd, file, before, beforeStat);
}

void testPersistenceFailureDoesNotPublishMetadata() {
  auto backend = std::make_unique<MetadataPersistenceBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  auto realBackend = reinterpret_cast<::backend_t>(backendHandle);
  // Exercise the virtual backend too. Its wrapper must not publish its own
  // File metadata if the real persistence hook rejects the same candidate.
  auto virtualBackend = wasmfs_create_icase_backend(realBackend);
  assert(virtualBackend);
  assert(wasmfs_create_directory(MountPath, 0700, virtualBackend) == 0);

  int fd = open(FilePath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(backendState->file);
  const auto before = getMetadata(backendState->file);
  struct stat beforeStat;
  assert(fstat(fd, &beforeStat) == 0);

  backendState->state.result = -EIO;

  int callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, FilePath, 0644, 0), EIO);
  assertRejectedBeforePublish(
    backendState, fd, backendState->file, before, beforeStat, callsBefore);

  callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(fchmod(fd, 0640), EIO);
  assertRejectedBeforePublish(
    backendState, fd, backendState->file, before, beforeStat, callsBefore);

  const timespec pathTimes[2] = {
    {42, 111000000},
    {43, 222000000},
  };
  callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(utimensat(AT_FDCWD, FilePath, pathTimes, 0), EIO);
  assertRejectedBeforePublish(
    backendState, fd, backendState->file, before, beforeStat, callsBefore);

  const timespec fdTimes[2] = {
    {44, 333000000},
    {45, 444000000},
  };
  callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(futimens(fd, fdTimes), EIO);
  assertRejectedBeforePublish(
    backendState, fd, backendState->file, before, beforeStat, callsBefore);

  // A backend hook that violates the negative-errno convention must also
  // fail closed instead of becoming a successful POSIX syscall.
  backendState->state.result = EIO;
  callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, FilePath, 0644, 0), EIO);
  assertRejectedBeforePublish(
    backendState, fd, backendState->file, before, beforeStat, callsBefore);
  backendState->state.result = -EIO;

  // A pair of UTIME_OMIT values requests no metadata mutation and must not
  // call a persistence backend that would otherwise reject the operation.
  const timespec omitTimes[2] = {
    {0, UTIME_OMIT},
    {0, UTIME_OMIT},
  };
  callsBefore = backendState->state.calls;
  assert(utimensat(AT_FDCWD, FilePath, omitTimes, 0) == 0);
  assert(backendState->state.calls == callsBefore);
  assertUnchanged(fd, backendState->file, before, beforeStat);

  // Accepting a complete candidate publishes all of its fields only after the
  // backend hook returns successfully.
  backendState->state.result = 0;
  assert(utimensat(AT_FDCWD, FilePath, pathTimes, 0) == 0);
  const auto afterTimes = getMetadata(backendState->file);
  assertSameMetadata(backendState->state.candidate, afterTimes);
  assert(afterTimes.mode == before.mode);
  assert(afterTimes.atime == 42111.0);
  assert(afterTimes.mtime == 43222.0);

  assert(close(fd) == 0);
  assert(unlink(FilePath) == 0);

  // Directories are Files too. Verify that their explicit mode and timestamp
  // setters share the same candidate-before-publish contract.
  assert(backendState->directory);
  int directoryFD = open(MountPath, O_RDONLY | O_DIRECTORY);
  assert(directoryFD >= 0);
  const auto directoryBefore = getMetadata(backendState->directory);
  struct stat directoryBeforeStat;
  assert(fstat(directoryFD, &directoryBeforeStat) == 0);
  backendState->state.result = -EIO;

  callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(fchmodat(AT_FDCWD, MountPath, 0755, 0), EIO);
  assertRejectedBeforePublish(backendState,
                              directoryFD,
                              backendState->directory,
                              directoryBefore,
                              directoryBeforeStat,
                              callsBefore);

  callsBefore = backendState->state.calls;
  errno = 0;
  expectFailure(futimens(directoryFD, fdTimes), EIO);
  assertRejectedBeforePublish(backendState,
                              directoryFD,
                              backendState->directory,
                              directoryBefore,
                              directoryBeforeStat,
                              callsBefore);

  assert(close(directoryFD) == 0);
  assert(wasmfs_unmount(MountPath) == 0);
}

} // anonymous namespace

int main() {
  testPersistenceFailureDoesNotPublishMetadata();
  puts("ok");
}
