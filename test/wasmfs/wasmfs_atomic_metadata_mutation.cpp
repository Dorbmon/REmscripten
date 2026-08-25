/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

using Metadata = wasmfs::File::Metadata;

struct MutationState {
  std::vector<uint8_t> data;
  int writeError = 0;
  int resizeError = 0;
  ssize_t shortWrite = -1;
  bool oversizedWriteResult = false;
  int atomicWriteCalls = 0;
  int atomicResizeCalls = 0;
  int legacyWriteCalls = 0;
  int legacyResizeCalls = 0;
  int commits = 0;
  Metadata visibleAtMutation = {};
  Metadata candidate = {};
  Metadata committed = {};
};

class AtomicMutationDataFile : public wasmfs::DataFile {
  MutationState& state;

  void observeMutation(const Metadata& metadata) {
    state.visibleAtMutation = {mode, atime, mtime, ctime};
    state.candidate = metadata;
  }

protected:
  int open(wasmfs::oflags_t) override { return 0; }
  int close() override { return 0; }

  ssize_t read(uint8_t* buffer, size_t length, off_t offset) override {
    if (offset >= static_cast<off_t>(state.data.size())) {
      return 0;
    }
    size_t available = state.data.size() - offset;
    size_t result = std::min(length, available);
    memcpy(buffer, state.data.data() + offset, result);
    return result;
  }

  // The required-backend dispatch must never reach these legacy split hooks.
  ssize_t write(const uint8_t*, size_t, off_t) override {
    ++state.legacyWriteCalls;
    return -EIO;
  }

  int setSize(off_t) override {
    ++state.legacyResizeCalls;
    return -EIO;
  }

  ssize_t writeWithMetadata(const uint8_t* buffer,
                            size_t length,
                            off_t offset,
                            const Metadata& metadata) override {
    ++state.atomicWriteCalls;
    observeMutation(metadata);
    if (state.writeError) {
      return state.writeError;
    }
    if (state.oversizedWriteResult) {
      // Deliberately do not mutate the fake durable image. The generic handle
      // must reject this malformed success and leave WasmFS metadata alone.
      return length + 1;
    }

    size_t result = state.shortWrite < 0
                      ? length
                      : std::min(length, size_t(state.shortWrite));
    auto next = state.data;
    size_t end = size_t(offset) + result;
    if (end > next.size()) {
      next.resize(end);
    }
    memcpy(next.data() + offset, buffer, result);
    state.data = std::move(next);
    state.committed = metadata;
    ++state.commits;
    return result;
  }

  int setSizeWithMetadata(off_t size, const Metadata& metadata) override {
    ++state.atomicResizeCalls;
    observeMutation(metadata);
    if (state.resizeError) {
      return state.resizeError;
    }
    state.data.resize(size);
    state.committed = metadata;
    ++state.commits;
    return 0;
  }

  int flush() override { return 0; }

  off_t getSize() override { return state.data.size(); }

public:
  AtomicMutationDataFile(mode_t mode,
                         wasmfs::backend_t backend,
                         MutationState& state)
    : DataFile(mode, backend), state(state) {}
};

class AtomicMutationBackend : public wasmfs::Backend {
public:
  MutationState state;
  AtomicMutationDataFile* file = nullptr;

  bool requiresAtomicMetadataMutations() const override { return true; }

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    auto result =
      std::make_shared<AtomicMutationDataFile>(mode, this, state);
    file = result.get();
    return result;
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDirectory>(mode, this);
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(std::move(target), this);
  }
};

struct MissingHookState {
  int legacyWriteCalls = 0;
  int legacyResizeCalls = 0;
};

// This class intentionally does not override writeWithMetadata() or
// setSizeWithMetadata(). It models a backend that declares atomic
// content/metadata transactions but has not implemented them yet.
class MissingAtomicHooksDataFile : public wasmfs::DataFile {
  MissingHookState& state;

protected:
  int open(wasmfs::oflags_t) override { return 0; }
  int close() override { return 0; }
  ssize_t read(uint8_t*, size_t, off_t) override { return 0; }
  ssize_t write(const uint8_t*, size_t, off_t) override {
    ++state.legacyWriteCalls;
    return -EIO;
  }
  int setSize(off_t) override {
    ++state.legacyResizeCalls;
    return -EIO;
  }
  int flush() override { return 0; }
  off_t getSize() override { return 0; }

public:
  MissingAtomicHooksDataFile(mode_t mode,
                             wasmfs::backend_t backend,
                             MissingHookState& state)
    : DataFile(mode, backend), state(state) {}
};

class MissingAtomicHooksBackend : public wasmfs::Backend {
public:
  MissingHookState state;

  bool requiresAtomicMetadataMutations() const override { return true; }

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<MissingAtomicHooksDataFile>(mode, this, state);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDirectory>(mode, this);
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(std::move(target), this);
  }
};

constexpr char MountPath[] = "/wasmfs-atomic-metadata-mutation";
constexpr char FilePath[] = "/wasmfs-atomic-metadata-mutation/file";
constexpr char MissingHooksMountPath[] = "/wasmfs-atomic-metadata-missing";
constexpr char MissingHooksFilePath[] =
  "/wasmfs-atomic-metadata-missing/file";

struct Snapshot {
  std::vector<uint8_t> data;
  Metadata metadata;
  int commits;
};

void assertSameMetadata(const Metadata& actual, const Metadata& expected) {
  assert(actual.mode == expected.mode);
  assert(actual.atime == expected.atime);
  assert(actual.mtime == expected.mtime);
  assert(actual.ctime == expected.ctime);
}

Metadata getMetadata(AtomicMutationDataFile* file) {
  return file->locked().getMetadata();
}

Snapshot snapshot(const AtomicMutationBackend& backend) {
  return {backend.state.data, getMetadata(backend.file), backend.state.commits};
}

void assertUnchanged(const AtomicMutationBackend& backend,
                     const Snapshot& before) {
  assert(backend.state.data == before.data);
  assertSameMetadata(getMetadata(backend.file), before.metadata);
  assert(backend.state.commits == before.commits);
}

void assertCommittedCandidate(const AtomicMutationBackend& backend,
                              const Metadata& before) {
  assertSameMetadata(backend.state.visibleAtMutation, before);
  assertSameMetadata(backend.state.committed, backend.state.candidate);
  assertSameMetadata(getMetadata(backend.file), backend.state.candidate);
  assert(backend.state.candidate.mtime >= before.mtime);
  assert(backend.state.candidate.ctime >= before.ctime);
}

void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void testRequiredAtomicDataMutations() {
  auto backend = std::make_unique<AtomicMutationBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  assert(wasmfs_create_directory(
           MountPath, 0700, reinterpret_cast<::backend_t>(backendHandle)) ==
         0);

  int fd = open(FilePath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(backendState->file);

  // A normal successful write takes the paired hook, never write(), and
  // publishes precisely the metadata image that the hook committed.
  assert(pwrite(fd, "seed", 4, 0) == 4);
  assert(backendState->state.atomicWriteCalls == 1);
  assert(backendState->state.legacyWriteCalls == 0);
  assert(backendState->state.data == std::vector<uint8_t>({'s', 'e', 'e', 'd'}));
  assertSameMetadata(getMetadata(backendState->file),
                     backendState->state.candidate);

  auto before = snapshot(*backendState);
  backendState->state.writeError = -ENOSPC;
  errno = 0;
  expectFailure(pwrite(fd, "full", 4, 0), ENOSPC);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyWriteCalls == 0);

  before = snapshot(*backendState);
  backendState->state.writeError = -ENOTSUP;
  errno = 0;
  expectFailure(write(fd, "unsupported", 11), ENOTSUP);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyWriteCalls == 0);

  before = snapshot(*backendState);
  backendState->state.writeError = 0;
  backendState->state.oversizedWriteResult = true;
  errno = 0;
  expectFailure(pwrite(fd, "bad", 3, 0), EIO);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyWriteCalls == 0);
  backendState->state.oversizedWriteResult = false;

  // A positive short write is a committed transaction for exactly the bytes
  // reported by the backend, and it publishes the matching candidate once.
  before = snapshot(*backendState);
  backendState->state.shortWrite = 2;
  assert(pwrite(fd, "WXYZ", 4, 4) == 2);
  assert(backendState->state.data ==
         std::vector<uint8_t>({'s', 'e', 'e', 'd', 'W', 'X'}));
  assertCommittedCandidate(*backendState, before.metadata);
  assert(backendState->state.legacyWriteCalls == 0);
  backendState->state.shortWrite = -1;

  // ftruncate cannot use setSize() for a required backend. Both error paths
  // leave the fake durable image and the WasmFS image unchanged.
  before = snapshot(*backendState);
  backendState->state.resizeError = -ENOSPC;
  errno = 0;
  expectFailure(ftruncate(fd, 3), ENOSPC);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyResizeCalls == 0);

  before = snapshot(*backendState);
  backendState->state.resizeError = -ENOTSUP;
  errno = 0;
  expectFailure(ftruncate(fd, 3), ENOTSUP);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyResizeCalls == 0);

  before = snapshot(*backendState);
  backendState->state.resizeError = 0;
  assert(ftruncate(fd, 3) == 0);
  assert(backendState->state.data == std::vector<uint8_t>({'s', 'e', 'e'}));
  assertCommittedCandidate(*backendState, before.metadata);
  assert(backendState->state.legacyResizeCalls == 0);

  // Historical WasmFS accepts a failed read-only O_TRUNC for legacy backends.
  // A required transaction backend must instead report the missing atomic
  // resize: treating it as a successful open would be false success.
  before = snapshot(*backendState);
  backendState->state.resizeError = -ENOTSUP;
  errno = 0;
  expectFailure(open(FilePath, O_RDONLY | O_TRUNC), ENOTSUP);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyResizeCalls == 0);

  before = snapshot(*backendState);
  backendState->state.resizeError = 0;
  int truncating = open(FilePath, O_WRONLY | O_TRUNC);
  assert(truncating >= 0);
  assert(backendState->state.data.empty());
  assertCommittedCandidate(*backendState, before.metadata);
  assert(backendState->state.legacyResizeCalls == 0);
  assert(close(truncating) == 0);

  // fallocate's no-op reservation modes remain unsupported. Its actual growth
  // path needs the paired resize transaction and propagates the hook error.
  before = snapshot(*backendState);
  backendState->state.resizeError = -ENOSPC;
  assert(posix_fallocate(fd, 0, 5) == ENOSPC);
  assertSameMetadata(backendState->state.visibleAtMutation, before.metadata);
  assertUnchanged(*backendState, before);
  assert(backendState->state.legacyResizeCalls == 0);

  before = snapshot(*backendState);
  backendState->state.resizeError = 0;
  assert(posix_fallocate(fd, 0, 5) == 0);
  assert(backendState->state.data == std::vector<uint8_t>(5, 0));
  assertCommittedCandidate(*backendState, before.metadata);
  assert(backendState->state.legacyResizeCalls == 0);

  assert(close(fd) == 0);
  assert(unlink(FilePath) == 0);
  assert(wasmfs_unmount(MountPath) == 0);
}

void testMissingAtomicHooksFailClosed() {
  auto backend = std::make_unique<MissingAtomicHooksBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  assert(wasmfs_create_directory(
           MissingHooksMountPath,
           0700,
           reinterpret_cast<::backend_t>(backendHandle)) == 0);

  int fd = open(MissingHooksFilePath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);

  // The capability itself is a promise to use the paired hooks. Their default
  // ENOTSUP must reach every data-mutating path without reaching legacy
  // write()/setSize() as a fallback.
  errno = 0;
  expectFailure(write(fd, "x", 1), ENOTSUP);
  assert(backendState->state.legacyWriteCalls == 0);

  errno = 0;
  expectFailure(ftruncate(fd, 1), ENOTSUP);
  assert(backendState->state.legacyResizeCalls == 0);

  errno = 0;
  expectFailure(open(MissingHooksFilePath, O_RDONLY | O_TRUNC), ENOTSUP);
  assert(backendState->state.legacyResizeCalls == 0);

  assert(posix_fallocate(fd, 0, 1) == ENOTSUP);
  assert(backendState->state.legacyResizeCalls == 0);

  assert(close(fd) == 0);
  assert(unlink(MissingHooksFilePath) == 0);
  assert(wasmfs_unmount(MissingHooksMountPath) == 0);
}

} // anonymous namespace

int main() {
  testRequiredAtomicDataMutations();
  testMissingAtomicHooksFailClosed();
  puts("ok");
}
