/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN_PTHREADS__
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/file_table.h"
#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

using Metadata = wasmfs::File::Metadata;
using NamespaceMutation = wasmfs::Directory::NamespaceMutation;

void assertSameMetadata(const Metadata& actual, const Metadata& expected) {
  assert(actual.mode == expected.mode);
  assert(actual.atime == expected.atime);
  assert(actual.mtime == expected.mtime);
  assert(actual.ctime == expected.ctime);
}

Metadata getMetadata(const std::shared_ptr<wasmfs::File>& file) {
  return file->locked().getMetadata();
}

void expectFailure(int result, int expectedErrno) {
  assert(result == -1);
  assert(errno == expectedErrno);
}

void assertMissing(const char* path) {
  errno = 0;
  assert(access(path, F_OK) == -1);
  assert(errno == ENOENT);
}

struct ObservedMutation {
  NamespaceMutation::Kind kind = NamespaceMutation::Kind::CreateDataFile;
  std::shared_ptr<wasmfs::Directory> sourceParent;
  std::string sourceName;
  std::shared_ptr<wasmfs::Directory> destinationParent;
  std::string destinationName;
  std::shared_ptr<wasmfs::File> subject;
  std::shared_ptr<wasmfs::File> replacement;
  std::optional<Metadata> sourceParentPostImage;
  std::optional<Metadata> destinationParentPostImage;
  std::optional<Metadata> subjectPostImage;
  std::optional<Metadata> replacementPostImage;
  std::optional<Metadata> sourceParentVisibleAtCommit;
  std::optional<Metadata> destinationParentVisibleAtCommit;
  std::optional<Metadata> subjectVisibleAtCommit;
  std::optional<Metadata> replacementVisibleAtCommit;
  std::shared_ptr<wasmfs::Directory> subjectParentAtCommit;
  std::shared_ptr<wasmfs::Directory> replacementParentAtCommit;
};

struct NamespaceState {
  int transactionError = 0;
  int transactionCalls = 0;
  int commits = 0;
  int legacyInsertDataFileCalls = 0;
  int legacyInsertDirectoryCalls = 0;
  int legacyInsertSymlinkCalls = 0;
  int legacyInsertMoveCalls = 0;
  int legacyRemoveChildCalls = 0;
  ObservedMutation last;
};

#ifdef __EMSCRIPTEN_PTHREADS__
// Coordinate a lookup that holds an ancestor directory's File lock with a
// related-parent rename. This is deliberately a test-only barrier: ordinary
// Directory::Handle::getChildWithError() invokes FakeNamespaceDirectory's
// getChild() while the ancestor handle is live, then locks the discovered
// child to cache it.
struct LookupRenameRace {
  std::mutex mutex;
  std::condition_variable condition;
  bool lookupEntered = false;
  bool writerReady = false;
  bool startWriter = false;
  bool releaseLookup = false;
  bool firstParentLockReported = false;
  bool releaseWriterAfterFirstParentLock = false;
  bool readerDone = false;
  bool writerDone = false;
  wasmfs::Directory* firstLockedParent = nullptr;
  int readerResult = -EIO;
  int writerResult = -EIO;
};

LookupRenameRace* activeLookupRenameRace = nullptr;
wasmfs::Directory* lookupRenameRaceOldParent = nullptr;
wasmfs::Directory* lookupRenameRaceNewParent = nullptr;

// RenameParentLocks calls this undefined-weak system hook immediately after
// taking its first actual parent handle. Block that writer until the test has
// verified the identity, so the reader can finish its P -> D cache operation
// while the writer holds P rather than D.
extern "C" void wasmfs_rename_first_parent_lock_for_testing(
  wasmfs::Directory* oldParent,
  wasmfs::Directory* newParent,
  wasmfs::Directory* firstParent) {
  auto* race = activeLookupRenameRace;
  if (!race || oldParent != lookupRenameRaceOldParent ||
      newParent != lookupRenameRaceNewParent) {
    return;
  }
  std::unique_lock lock(race->mutex);
  race->firstLockedParent = firstParent;
  race->firstParentLockReported = true;
  race->condition.notify_all();
  race->condition.wait(
    lock, [&] { return race->releaseWriterAfterFirstParentLock; });
}
#endif

// This provides a fake durable namespace separate from WasmFS's dcache and
// parent links. Its legacy hooks are deliberately unusable: an atomic-capable
// backend must reach commitNamespaceMutation(), never one of these split
// callbacks.
class FakeNamespaceDirectory : public wasmfs::Directory {
  std::map<std::string, std::shared_ptr<wasmfs::File>> entries;

#ifdef __EMSCRIPTEN_PTHREADS__
  LookupRenameRace* lookupRenameRace = nullptr;
  std::string lookupRenameRaceName;
#endif

protected:
  NamespaceState& state;

  std::shared_ptr<wasmfs::File> getChild(const std::string& name) override {
#ifdef __EMSCRIPTEN_PTHREADS__
    if (lookupRenameRace && name == lookupRenameRaceName) {
      std::unique_lock lock(lookupRenameRace->mutex);
      lookupRenameRace->lookupEntered = true;
      lookupRenameRace->condition.notify_all();
      lookupRenameRace->condition.wait(
        lock, [&] { return lookupRenameRace->releaseLookup; });
    }
#endif
    auto entry = entries.find(name);
    return entry == entries.end() ? nullptr : entry->second;
  }

  std::shared_ptr<wasmfs::DataFile>
  insertDataFile(const std::string&, mode_t) override {
    ++state.legacyInsertDataFileCalls;
    return nullptr;
  }

  std::shared_ptr<wasmfs::Directory>
  insertDirectory(const std::string&, mode_t) override {
    ++state.legacyInsertDirectoryCalls;
    return nullptr;
  }

  std::shared_ptr<wasmfs::Symlink>
  insertSymlink(const std::string&, const std::string&) override {
    ++state.legacyInsertSymlinkCalls;
    return nullptr;
  }

  int insertMove(const std::string&, std::shared_ptr<wasmfs::File>) override {
    ++state.legacyInsertMoveCalls;
    return -EIO;
  }

  int removeChild(const std::string&) override {
    ++state.legacyRemoveChildCalls;
    return -EIO;
  }

  ssize_t getNumEntries() override { return entries.size(); }

  MaybeEntries getEntries() override {
    std::vector<Entry> result;
    result.reserve(entries.size());
    for (const auto& [name, child] : entries) {
      result.push_back({name, child->kind, child->getIno()});
    }
    return {result};
  }

public:
  FakeNamespaceDirectory(mode_t mode,
                         wasmfs::backend_t backend,
                         NamespaceState& state)
    : Directory(mode, backend), state(state) {}

  std::shared_ptr<wasmfs::File> storedChild(const std::string& name) const {
    auto entry = entries.find(name);
    return entry == entries.end() ? nullptr : entry->second;
  }

  void insertDurable(const std::string& name,
                     std::shared_ptr<wasmfs::File> child) {
    auto [_, inserted] = entries.insert({name, std::move(child)});
    assert(inserted);
  }

  std::shared_ptr<wasmfs::File> eraseDurable(const std::string& name) {
    auto entry = entries.find(name);
    assert(entry != entries.end());
    auto child = entry->second;
    entries.erase(entry);
    return child;
  }

  // Seed the fake durable state without exercising a mutation callback. This
  // lets the missing-hook test reach unlink, rmdir, and rename directly.
  void seed(const std::string& name, std::shared_ptr<wasmfs::File> child) {
    assert(!child->locked().getParent());
    insertDurable(name, child);
    child->locked().setParent(shared_from_this()->cast<wasmfs::Directory>());
  }

#ifdef __EMSCRIPTEN_PTHREADS__
  void pauseLookupForRenameRace(const std::string& name,
                                LookupRenameRace& race) {
    assert(!lookupRenameRace);
    lookupRenameRace = &race;
    lookupRenameRaceName = name;
  }

  void stopLookupRenameRace() {
    lookupRenameRace = nullptr;
    lookupRenameRaceName.clear();
  }
#endif
};

class AtomicNamespaceDirectory : public FakeNamespaceDirectory {
  void observe(const NamespaceMutation& mutation) {
    auto& observed = state.last;
    observed = {};
    observed.kind = mutation.kind;
    observed.sourceParent = mutation.sourceParent;
    observed.sourceName = mutation.sourceName;
    observed.destinationParent = mutation.destinationParent;
    observed.destinationName = mutation.destinationName;
    observed.subject = mutation.subject;
    observed.replacement = mutation.replacement;
    observed.sourceParentPostImage = mutation.sourceParentPostImage;
    observed.destinationParentPostImage = mutation.destinationParentPostImage;
    observed.subjectPostImage = mutation.subjectPostImage;
    observed.replacementPostImage = mutation.replacementPostImage;
    if (mutation.sourceParent) {
      observed.sourceParentVisibleAtCommit =
        getMetadata(mutation.sourceParent);
    }
    if (mutation.destinationParent) {
      observed.destinationParentVisibleAtCommit =
        getMetadata(mutation.destinationParent);
    }
    if (mutation.subject) {
      observed.subjectVisibleAtCommit = getMetadata(mutation.subject);
      observed.subjectParentAtCommit = mutation.subject->locked().getParent();
    }
    if (mutation.replacement) {
      observed.replacementVisibleAtCommit = getMetadata(mutation.replacement);
      observed.replacementParentAtCommit =
        mutation.replacement->locked().getParent();
    }
  }

  static std::shared_ptr<FakeNamespaceDirectory>
  asFakeDirectory(const std::shared_ptr<wasmfs::Directory>& directory) {
    assert(directory);
    return std::static_pointer_cast<FakeNamespaceDirectory>(directory);
  }

protected:
  int commitNamespaceMutation(const NamespaceMutation& mutation) override {
    ++state.transactionCalls;
    observe(mutation);
    if (state.transactionError) {
      return state.transactionError;
    }

    switch (mutation.kind) {
      case NamespaceMutation::Kind::CreateDataFile:
      case NamespaceMutation::Kind::CreateDirectory:
      case NamespaceMutation::Kind::CreateSymlink: {
        assert(!mutation.sourceParent);
        assert(mutation.destinationParent);
        assert(mutation.subject);
        assert(mutation.destinationParentPostImage);
        assert(mutation.subjectPostImage);
        assert(!mutation.subject->locked().getParent());
        auto destination = asFakeDirectory(mutation.destinationParent);
        assert(!destination->storedChild(mutation.destinationName));
        destination->insertDurable(mutation.destinationName, mutation.subject);
        break;
      }
      case NamespaceMutation::Kind::Unlink:
      case NamespaceMutation::Kind::RemoveDirectory: {
        assert(mutation.sourceParent);
        assert(!mutation.destinationParent);
        assert(mutation.subject);
        assert(mutation.sourceParentPostImage);
        assert(mutation.subjectPostImage);
        auto source = asFakeDirectory(mutation.sourceParent);
        assert(source->storedChild(mutation.sourceName) == mutation.subject);
        assert(source->eraseDurable(mutation.sourceName) == mutation.subject);
        break;
      }
      case NamespaceMutation::Kind::Rename: {
        assert(mutation.sourceParent);
        assert(mutation.destinationParent);
        assert(mutation.subject);
        assert(mutation.sourceParentPostImage);
        assert(mutation.subjectPostImage);
        auto source = asFakeDirectory(mutation.sourceParent);
        auto destination = asFakeDirectory(mutation.destinationParent);
        assert(source->storedChild(mutation.sourceName) == mutation.subject);
        if (mutation.replacement) {
          assert(destination->storedChild(mutation.destinationName) ==
                 mutation.replacement);
          assert(mutation.replacementPostImage);
        } else {
          assert(!destination->storedChild(mutation.destinationName));
          assert(!mutation.replacementPostImage);
        }
        assert(source->eraseDurable(mutation.sourceName) == mutation.subject);
        if (mutation.replacement) {
          assert(destination->eraseDurable(mutation.destinationName) ==
                 mutation.replacement);
        }
        destination->insertDurable(mutation.destinationName, mutation.subject);
        break;
      }
    }

    ++state.commits;
    return 0;
  }

public:
  AtomicNamespaceDirectory(mode_t mode,
                           wasmfs::backend_t backend,
                           NamespaceState& state)
    : FakeNamespaceDirectory(mode, backend, state) {}
};

// This intentionally leaves commitNamespaceMutation() at Directory's default
// ENOTSUP implementation. It models a backend that opts into the capability
// before it has supplied the required transaction hook.
class MissingAtomicNamespaceDirectory : public FakeNamespaceDirectory {
public:
  using FakeNamespaceDirectory::FakeNamespaceDirectory;
};

class AtomicNamespaceBackend : public wasmfs::Backend {
public:
  NamespaceState state;
  std::shared_ptr<AtomicNamespaceDirectory> root;
  std::shared_ptr<wasmfs::File> lastCreated;
  wasmfs::backend_t fileCandidateBackend = wasmfs::NullBackend;
  bool returnLinkedFileCandidate = false;

  bool requiresAtomicNamespaceMutations() const override { return true; }

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    auto result = std::make_shared<wasmfs::MemoryDataFile>(
      mode, fileCandidateBackend ? fileCandidateBackend : this);
    if (returnLinkedFileCandidate && root) {
      result->locked().setParent(root);
    }
    lastCreated = result;
    return result;
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto result =
      std::make_shared<AtomicNamespaceDirectory>(mode, this, state);
    if (!root) {
      root = result;
    }
    lastCreated = result;
    return result;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    auto result = std::make_shared<wasmfs::MemorySymlink>(target, this);
    lastCreated = result;
    return result;
  }
};

class MissingAtomicNamespaceBackend : public wasmfs::Backend {
public:
  NamespaceState state;
  std::shared_ptr<MissingAtomicNamespaceDirectory> root;
  std::shared_ptr<wasmfs::File> lastCreated;

  bool requiresAtomicNamespaceMutations() const override { return true; }

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    auto result = std::make_shared<wasmfs::MemoryDataFile>(mode, this);
    lastCreated = result;
    return result;
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto result =
      std::make_shared<MissingAtomicNamespaceDirectory>(mode, this, state);
    if (!root) {
      root = result;
    }
    lastCreated = result;
    return result;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    auto result = std::make_shared<wasmfs::MemorySymlink>(target, this);
    lastCreated = result;
    return result;
  }
};

void assertLegacyHooksUnused(const NamespaceState& state) {
  assert(state.legacyInsertDataFileCalls == 0);
  assert(state.legacyInsertDirectoryCalls == 0);
  assert(state.legacyInsertSymlinkCalls == 0);
  assert(state.legacyInsertMoveCalls == 0);
  assert(state.legacyRemoveChildCalls == 0);
}

void assertPublished(const std::shared_ptr<wasmfs::File>& file,
                     const std::optional<Metadata>& expected) {
  assert(expected);
  assertSameMetadata(getMetadata(file), *expected);
}

void assertCreatePublication(const NamespaceState& state,
                             NamespaceMutation::Kind kind,
                             const std::shared_ptr<wasmfs::Directory>& parent,
                             const std::shared_ptr<wasmfs::File>& child,
                             const Metadata& parentBefore) {
  const auto& mutation = state.last;
  assert(mutation.kind == kind);
  assert(mutation.destinationParent == parent);
  assert(mutation.subject == child);
  assert(!mutation.subjectParentAtCommit);
  assert(mutation.destinationParentPostImage);
  assert(mutation.subjectPostImage);
  assert(mutation.destinationParentVisibleAtCommit);
  assert(mutation.subjectVisibleAtCommit);
  assertSameMetadata(*mutation.destinationParentVisibleAtCommit, parentBefore);
  assertSameMetadata(*mutation.subjectVisibleAtCommit,
                     *mutation.subjectPostImage);
  assertPublished(parent, mutation.destinationParentPostImage);
  assertPublished(child, mutation.subjectPostImage);
}

void createFile(const char* path) {
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);
}

constexpr char AtomicMountPath[] = "/wasmfs-atomic-namespace";
constexpr char FailedCreatePath[] = "/wasmfs-atomic-namespace/failed";
constexpr char ForeignCandidatePath[] =
  "/wasmfs-atomic-namespace/foreign-candidate";
constexpr char LinkedCandidatePath[] =
  "/wasmfs-atomic-namespace/linked-candidate";
constexpr char FilePath[] = "/wasmfs-atomic-namespace/file";
constexpr char DirectoryPath[] = "/wasmfs-atomic-namespace/directory";
constexpr char SymlinkPath[] = "/wasmfs-atomic-namespace/link";
constexpr char SameSourcePath[] = "/wasmfs-atomic-namespace/same-source";
constexpr char SameReplacementPath[] =
  "/wasmfs-atomic-namespace/same-replacement";
constexpr char CrossSourceDirectoryPath[] =
  "/wasmfs-atomic-namespace/cross-source-directory";
constexpr char CrossDestinationDirectoryPath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory";
constexpr char CrossSourcePath[] =
  "/wasmfs-atomic-namespace/cross-source-directory/source";
constexpr char CrossReplacementPath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory/replacement";
constexpr char DetachedDescendantPath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory/detached-descendant";
constexpr char DetachedVictimPath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory/detached-descendant/"
  "victim";
constexpr char DetachedEmptyDirectoryPath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory/detached-descendant/"
  "empty";
constexpr char DetachedRenameSourcePath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory/detached-descendant/"
  "rename-source";
constexpr char NestedMountPath[] =
  "/wasmfs-atomic-namespace/cross-destination-directory/detached-descendant/"
  "nested-mount";
constexpr char AttachedRenameSourcePath[] = "/wasmfs-atomic-attached-source";
constexpr char AttachedRenameDestinationPath[] =
  "/wasmfs-atomic-attached-destination";

#ifdef __EMSCRIPTEN_PTHREADS__
constexpr char RenameRaceParentPath[] =
  "/wasmfs-atomic-namespace/rename-race-parent";

int installDirectoryFD(const std::shared_ptr<wasmfs::Directory>& directory) {
  std::shared_ptr<wasmfs::OpenFileState> openFile;
  assert(wasmfs::OpenFileState::create(directory, O_RDONLY, openFile) == 0);
  auto fileTable = wasmfs::wasmFS.getFileTable().locked();
  auto fd = fileTable.addEntry(std::move(openFile));
  assert(fd != static_cast<__wasi_fd_t>(-EMFILE));
  return static_cast<int>(fd);
}

// Exercise the ordering required when a lookup holds P and then caches its
// uncached child D while a rename moves a file from D into P. A child-first
// rename holds D and waits for P, then deadlocks when the lookup tries to lock
// D. RenameParentLocks must instead acquire P before D (or wait for P while
// determining that relationship), allowing the lookup to finish first.
void testRelatedParentRenameLockOrder(AtomicNamespaceBackend* backend,
                                      const std::shared_ptr<
                                        AtomicNamespaceDirectory>& root) {
  assert(mkdir(RenameRaceParentPath, 0700) == 0);
  auto parent = std::static_pointer_cast<FakeNamespaceDirectory>(
    root->storedChild("rename-race-parent"));
  assert(parent);

  // Seed D directly in the fake durable namespace. In particular, do not add
  // it to P's dcache: the reader below must traverse P -> D while P is held.
  auto child = std::static_pointer_cast<FakeNamespaceDirectory>(
    backend->createDirectory(0700));
  parent->seed("child-dir", child);
  auto source = backend->createFile(0600);
  child->seed("source", source);

  // Synthetic directory descriptors avoid a preparatory pathname lookup of
  // P/child-dir that would populate the dcache before the controlled reader.
  const int parentFD = installDirectoryFD(parent);
  const int childFD = installDirectoryFD(child);

  LookupRenameRace race;
  parent->pauseLookupForRenameRace("child-dir", race);
  activeLookupRenameRace = &race;
  lookupRenameRaceOldParent = child.get();
  lookupRenameRaceNewParent = parent.get();
  const int transactionCallsBefore = backend->state.transactionCalls;

  std::thread reader([&] {
    int result = openat(parentFD, "child-dir", O_RDONLY | O_DIRECTORY);
    if (result >= 0) {
      assert(close(result) == 0);
      result = 0;
    } else {
      result = -errno;
    }
    std::lock_guard lock(race.mutex);
    race.readerResult = result;
    race.readerDone = true;
    race.condition.notify_all();
  });

  {
    std::unique_lock lock(race.mutex);
    assert(race.condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return race.lookupEntered;
    }));
  }

  std::thread writer([&] {
    {
      std::unique_lock lock(race.mutex);
      race.writerReady = true;
      race.condition.notify_all();
      race.condition.wait(lock, [&] { return race.startWriter; });
    }
    const int result = renameat(childFD, "source", parentFD, "renamed");
    std::lock_guard lock(race.mutex);
    race.writerResult = result == 0 ? 0 : -errno;
    race.writerDone = true;
    race.condition.notify_all();
  });

  {
    std::unique_lock lock(race.mutex);
    assert(race.condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return race.writerReady;
    }));
    race.startWriter = true;
    race.releaseLookup = true;
    race.condition.notify_all();
  }

  {
    std::unique_lock lock(race.mutex);
    assert(race.condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return race.firstParentLockReported;
    }));
    // D is the old parent and P is the new, ancestor parent. The generic
    // pair-lock rule must make P the first actual locked parent. This check is
    // deterministic even if either worker runs immediately after the barrier:
    // a historical child-first implementation reports D instead.
    assert(race.firstLockedParent == parent.get());
    assert(race.condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return race.readerDone;
    }));
    race.releaseWriterAfterFirstParentLock = true;
    race.condition.notify_all();
  }

  // Do not let a later lock-order regression leave the browser test worker
  // hung. The first-lock identity assertion above detects D -> P acquisition
  // before this bounded completion wait can form an ABBA deadlock.
  {
    std::unique_lock lock(race.mutex);
    assert(race.condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return race.readerDone && race.writerDone;
    }));
  }
  reader.join();
  writer.join();
  parent->stopLookupRenameRace();
  activeLookupRenameRace = nullptr;
  lookupRenameRaceOldParent = nullptr;
  lookupRenameRaceNewParent = nullptr;

  assert(race.readerResult == 0);
  assert(race.writerResult == 0);
  assert(child->storedChild("source") == nullptr);
  assert(parent->storedChild("renamed") == source);
  assert(source->locked().getParent() == parent);
  assert(backend->state.transactionCalls == transactionCallsBefore + 1);
  assert(close(childFD) == 0);
  assert(close(parentFD) == 0);
}
#endif

void testAtomicNamespaceMutations() {
  auto backend = std::make_unique<AtomicNamespaceBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  assert(wasmfs_create_directory(
           AtomicMountPath, 0700, reinterpret_cast<::backend_t>(backendHandle)) ==
         0);
  auto root = backendState->root;
  assert(root);

  // An error from the durable transaction must leave no cache entry, parent
  // link, or parent metadata update visible in WasmFS.
  auto rootBefore = getMetadata(root);
  backendState->state.transactionError = -ENOSPC;
  errno = 0;
  expectFailure(open(FailedCreatePath, O_CREAT | O_EXCL | O_RDWR, 0600),
                ENOSPC);
  assert(!root->storedChild("failed"));
  assert(backendState->lastCreated);
  assert(!backendState->lastCreated->locked().getParent());
  assertSameMetadata(getMetadata(root), rootBefore);
  assertMissing(FailedCreatePath);
  assertLegacyHooksUnused(backendState->state);

  // An atomic create factory may allocate a private candidate, but it cannot
  // hand generic WasmFS another backend's object or an already-linked one.
  // Both preconditions fail before the durable hook or any parent publication.
  rootBefore = getMetadata(root);
  int factoryTransactionCallsBefore = backendState->state.transactionCalls;
  auto foreignBackend = wasmfs::createMemoryBackend();
  backendState->fileCandidateBackend = foreignBackend;
  errno = 0;
  expectFailure(open(ForeignCandidatePath, O_CREAT | O_EXCL | O_RDWR, 0600),
                EIO);
  assert(backendState->lastCreated);
  assert(backendState->lastCreated->getBackend() == foreignBackend);
  assert(!backendState->lastCreated->locked().getParent());
  assert(!root->storedChild("foreign-candidate"));
  assertSameMetadata(getMetadata(root), rootBefore);
  assert(backendState->state.transactionCalls ==
         factoryTransactionCallsBefore);
  backendState->fileCandidateBackend = wasmfs::NullBackend;

  backendState->returnLinkedFileCandidate = true;
  errno = 0;
  expectFailure(open(LinkedCandidatePath, O_CREAT | O_EXCL | O_RDWR, 0600),
                EIO);
  assert(backendState->lastCreated);
  assert(backendState->lastCreated->locked().getParent() == root);
  assert(!root->storedChild("linked-candidate"));
  assertSameMetadata(getMetadata(root), rootBefore);
  assert(backendState->state.transactionCalls ==
         factoryTransactionCallsBefore);
  backendState->lastCreated->locked().setParent(nullptr);
  backendState->returnLinkedFileCandidate = false;

  // All three creation kinds receive both complete images and publish them
  // only after the fake durable namespace has committed the request.
  backendState->state.transactionError = 0;
  rootBefore = getMetadata(root);
  createFile(FilePath);
  auto file = root->storedChild("file");
  assert(file && file->is<wasmfs::DataFile>());
  assert(file->locked().getParent() == root);
  assertCreatePublication(backendState->state,
                          NamespaceMutation::Kind::CreateDataFile,
                          root,
                          file,
                          rootBefore);

  rootBefore = getMetadata(root);
  assert(mkdir(DirectoryPath, 0700) == 0);
  auto directory = root->storedChild("directory");
  assert(directory && directory->is<wasmfs::Directory>());
  assert(directory->locked().getParent() == root);
  assertCreatePublication(backendState->state,
                          NamespaceMutation::Kind::CreateDirectory,
                          root,
                          directory,
                          rootBefore);

  rootBefore = getMetadata(root);
  assert(symlink("target", SymlinkPath) == 0);
  auto link = root->storedChild("link");
  assert(link && link->is<wasmfs::Symlink>());
  assert(link->cast<wasmfs::Symlink>()->getTarget() == "target");
  assert(link->locked().getParent() == root);
  assertCreatePublication(backendState->state,
                          NamespaceMutation::Kind::CreateSymlink,
                          root,
                          link,
                          rootBefore);

  // A failed unlink must not detach the object or publish either candidate.
  // Keep a descriptor open to prove the removed inode receives its ctime image
  // after the durable erase succeeds.
  int unlinkedFd = open(FilePath, O_RDONLY);
  assert(unlinkedFd >= 0);
  auto fileBefore = getMetadata(file);
  rootBefore = getMetadata(root);
  backendState->state.transactionError = -ENOSPC;
  errno = 0;
  expectFailure(unlink(FilePath), ENOSPC);
  assert(root->storedChild("file") == file);
  assert(file->locked().getParent() == root);
  assertSameMetadata(getMetadata(file), fileBefore);
  assertSameMetadata(getMetadata(root), rootBefore);
  assertSameMetadata(*backendState->state.last.sourceParentVisibleAtCommit,
                     rootBefore);

  backendState->state.transactionError = 0;
  assert(unlink(FilePath) == 0);
  const auto unlinkMutation = backendState->state.last;
  assert(unlinkMutation.kind == NamespaceMutation::Kind::Unlink);
  assert(unlinkMutation.subject == file);
  assert(unlinkMutation.sourceParentPostImage);
  assert(unlinkMutation.subjectPostImage);
  assert(!root->storedChild("file"));
  assert(!file->locked().getParent());
  assertPublished(root, unlinkMutation.sourceParentPostImage);
  assertPublished(file, unlinkMutation.subjectPostImage);
  assert(getMetadata(file).mtime == fileBefore.mtime);
  assert(getMetadata(file).ctime >= fileBefore.ctime);
  assert(close(unlinkedFd) == 0);

  // rmdir follows the same failure and publication order as unlink, including
  // ctime for an open directory descriptor.
  int removedDirectoryFd = open(DirectoryPath, O_RDONLY | O_DIRECTORY);
  assert(removedDirectoryFd >= 0);
  auto directoryBefore = getMetadata(directory);
  rootBefore = getMetadata(root);
  backendState->state.transactionError = -ENOSPC;
  errno = 0;
  expectFailure(rmdir(DirectoryPath), ENOSPC);
  assert(root->storedChild("directory") == directory);
  assert(directory->locked().getParent() == root);
  assertSameMetadata(getMetadata(directory), directoryBefore);
  assertSameMetadata(getMetadata(root), rootBefore);

  backendState->state.transactionError = 0;
  assert(rmdir(DirectoryPath) == 0);
  const auto rmdirMutation = backendState->state.last;
  assert(rmdirMutation.kind == NamespaceMutation::Kind::RemoveDirectory);
  assert(rmdirMutation.subject == directory);
  assert(rmdirMutation.sourceParentPostImage);
  assert(rmdirMutation.subjectPostImage);
  assert(!root->storedChild("directory"));
  assert(!directory->locked().getParent());
  assertPublished(root, rmdirMutation.sourceParentPostImage);
  assertPublished(directory, rmdirMutation.subjectPostImage);
  assert(getMetadata(directory).mtime == directoryBefore.mtime);
  assert(getMetadata(directory).ctime >= directoryBefore.ctime);
  assert(close(removedDirectoryFd) == 0);

  // Same-parent replacement has one parent post-image and a ctime-only image
  // for the moved inode. Failed transactions leave both entries unchanged.
  createFile(SameSourcePath);
  createFile(SameReplacementPath);
  auto sameSource = root->storedChild("same-source");
  auto sameReplacement = root->storedChild("same-replacement");
  auto sameSourceBefore = getMetadata(sameSource);
  auto sameReplacementBefore = getMetadata(sameReplacement);
  rootBefore = getMetadata(root);
  backendState->state.transactionError = -ENOSPC;
  errno = 0;
  expectFailure(rename(SameSourcePath, SameReplacementPath), ENOSPC);
  assert(root->storedChild("same-source") == sameSource);
  assert(root->storedChild("same-replacement") == sameReplacement);
  assert(sameSource->locked().getParent() == root);
  assert(sameReplacement->locked().getParent() == root);
  assertSameMetadata(getMetadata(sameSource), sameSourceBefore);
  assertSameMetadata(getMetadata(sameReplacement), sameReplacementBefore);
  assertSameMetadata(getMetadata(root), rootBefore);

  backendState->state.transactionError = 0;
  assert(rename(SameSourcePath, SameReplacementPath) == 0);
  const auto sameRenameMutation = backendState->state.last;
  assert(sameRenameMutation.kind == NamespaceMutation::Kind::Rename);
  assert(sameRenameMutation.sourceParent == root);
  assert(sameRenameMutation.destinationParent == root);
  assert(sameRenameMutation.sourceParentPostImage);
  assert(!sameRenameMutation.destinationParentPostImage);
  assert(sameRenameMutation.subjectPostImage);
  assert(sameRenameMutation.replacementPostImage);
  assert(sameRenameMutation.replacement == sameReplacement);
  assert(root->storedChild("same-replacement") == sameSource);
  assert(!root->storedChild("same-source"));
  assert(!sameReplacement->locked().getParent());
  assertPublished(root, sameRenameMutation.sourceParentPostImage);
  assertPublished(sameSource, sameRenameMutation.subjectPostImage);
  assertPublished(sameReplacement, sameRenameMutation.replacementPostImage);
  assert(getMetadata(sameSource).mtime == sameSourceBefore.mtime);
  assert(getMetadata(sameSource).ctime >= sameSourceBefore.ctime);
  assert(getMetadata(sameReplacement).mtime == sameReplacementBefore.mtime);
  assert(getMetadata(sameReplacement).ctime >= sameReplacementBefore.ctime);

  // Cross-parent replacement supplies two independently complete directory
  // post-images plus the moved inode ctime image.
  assert(mkdir(CrossSourceDirectoryPath, 0700) == 0);
  assert(mkdir(CrossDestinationDirectoryPath, 0700) == 0);
  auto crossSourceDirectory = std::static_pointer_cast<FakeNamespaceDirectory>(
    root->storedChild("cross-source-directory"));
  auto crossDestinationDirectory =
    std::static_pointer_cast<FakeNamespaceDirectory>(
      root->storedChild("cross-destination-directory"));
  createFile(CrossSourcePath);
  createFile(CrossReplacementPath);
  auto crossSource = crossSourceDirectory->storedChild("source");
  auto crossReplacement = crossDestinationDirectory->storedChild("replacement");
  auto crossSourceBefore = getMetadata(crossSource);
  auto crossReplacementBefore = getMetadata(crossReplacement);
  auto sourceParentBefore = getMetadata(crossSourceDirectory);
  auto destinationParentBefore = getMetadata(crossDestinationDirectory);
  assert(rename(CrossSourcePath, CrossReplacementPath) == 0);
  const auto crossRenameMutation = backendState->state.last;
  assert(crossRenameMutation.kind == NamespaceMutation::Kind::Rename);
  assert(crossRenameMutation.sourceParent == crossSourceDirectory);
  assert(crossRenameMutation.destinationParent == crossDestinationDirectory);
  assert(crossRenameMutation.sourceParentPostImage);
  assert(crossRenameMutation.destinationParentPostImage);
  assert(crossRenameMutation.subjectPostImage);
  assert(crossRenameMutation.replacementPostImage);
  assert(crossRenameMutation.replacement == crossReplacement);
  assert(crossRenameMutation.sourceParentVisibleAtCommit);
  assert(crossRenameMutation.destinationParentVisibleAtCommit);
  assertSameMetadata(*crossRenameMutation.sourceParentVisibleAtCommit,
                     sourceParentBefore);
  assertSameMetadata(*crossRenameMutation.destinationParentVisibleAtCommit,
                     destinationParentBefore);
  assert(!crossSourceDirectory->storedChild("source"));
  assert(crossDestinationDirectory->storedChild("replacement") ==
         crossSource);
  assert(crossSource->locked().getParent() == crossDestinationDirectory);
  assert(!crossReplacement->locked().getParent());
  assertPublished(crossSourceDirectory,
                  crossRenameMutation.sourceParentPostImage);
  assertPublished(crossDestinationDirectory,
                  crossRenameMutation.destinationParentPostImage);
  assertPublished(crossSource, crossRenameMutation.subjectPostImage);
  assertPublished(crossReplacement, crossRenameMutation.replacementPostImage);
  assert(getMetadata(crossSource).mtime == crossSourceBefore.mtime);
  assert(getMetadata(crossSource).ctime >= crossSourceBefore.ctime);
  assert(getMetadata(crossReplacement).mtime == crossReplacementBefore.mtime);
  assert(getMetadata(crossReplacement).ctime >= crossReplacementBefore.ctime);

#ifdef __EMSCRIPTEN_PTHREADS__
  testRelatedParentRenameLockOrder(backendState, root);
#endif

  // A descriptor retained for a descendant of an unmounted tree can still
  // read existing files, but must not make any namespace change through that
  // stale ancestry. Seed each operation type while the mount is attached.
  assert(mkdir(DetachedDescendantPath, 0700) == 0);
  auto detachedDescendant = std::static_pointer_cast<FakeNamespaceDirectory>(
    crossDestinationDirectory->storedChild("detached-descendant"));
  assert(detachedDescendant);
  createFile(DetachedVictimPath);
  assert(mkdir(DetachedEmptyDirectoryPath, 0700) == 0);
  createFile(DetachedRenameSourcePath);
  createFile(AttachedRenameSourcePath);
  // A nested mount is cache-only in the fake directory. It exercises the
  // public wasmfs_unmount route separately from the file/dir entry mutations.
  assert(wasmfs_create_directory(
           NestedMountPath,
           0700,
           reinterpret_cast<::backend_t>(foreignBackend)) == 0);
  auto detachedVictim = detachedDescendant->storedChild("victim");
  auto detachedEmpty = detachedDescendant->storedChild("empty");
  auto detachedRenameSource =
    detachedDescendant->storedChild("rename-source");
  auto nestedMount =
    detachedDescendant->locked().getChild("nested-mount")->dynCast<
      wasmfs::Directory>();
  assert(detachedVictim && detachedEmpty && detachedRenameSource && nestedMount);
  int victimWriter = open(DetachedVictimPath, O_WRONLY);
  assert(victimWriter >= 0);
  assert(write(victimWriter, "x", 1) == 1);
  assert(close(victimWriter) == 0);

  int detachedDirfd =
    open(DetachedDescendantPath, O_RDONLY | O_DIRECTORY);
  assert(detachedDirfd >= 0);
  assert(wasmfs_unmount(AtomicMountPath) == 0);
  assert(!root->locked().getParent());
  int detachedTransactionCallsBefore = backendState->state.transactionCalls;
  auto rootBeforeDetachAttempts = getMetadata(root);
  auto crossDestinationBeforeDetachAttempts =
    getMetadata(crossDestinationDirectory);
  auto detachedDescendantBeforeDetachAttempts = getMetadata(detachedDescendant);
  auto detachedVictimBeforeDetachAttempts = getMetadata(detachedVictim);
  auto detachedEmptyBeforeDetachAttempts = getMetadata(detachedEmpty);
  auto detachedRenameSourceBeforeDetachAttempts =
    getMetadata(detachedRenameSource);

  // The error-bearing helper retains its immediate detached-destination EPERM
  // check as a defensive internal precondition.
  {
    auto lockedSourceDirectory = crossDestinationDirectory->locked();
    auto lockedRoot = root->locked();
    assert(lockedRoot.insertMoveWithNamespaceTransaction(
             lockedSourceDirectory,
             "replacement",
             "detached-rename-destination",
             crossSource,
             nullptr) == -EPERM);
  }
  assert(backendState->state.transactionCalls ==
         detachedTransactionCallsBefore);

  // Existing reads through a retained descriptor remain usable after unmount.
  int existingFd = openat(detachedDirfd, "victim", O_RDONLY);
  assert(existingFd >= 0);
  char byte = 0;
  assert(read(existingFd, &byte, 1) == 1);
  assert(byte == 'x');
  assert(close(existingFd) == 0);

  errno = 0;
  expectFailure(openat(detachedDirfd,
                       "created",
                       O_CREAT | O_EXCL | O_RDWR,
                       0600),
                ENOENT);
  errno = 0;
  expectFailure(mkdirat(detachedDirfd, "created-directory", 0700), ENOENT);
  errno = 0;
  expectFailure(symlinkat("target", detachedDirfd, "created-link"), ENOENT);
  errno = 0;
  expectFailure(unlinkat(detachedDirfd, "victim", 0), ENOENT);
  errno = 0;
  expectFailure(unlinkat(detachedDirfd, "empty", AT_REMOVEDIR), ENOENT);

  // Preserve legacy rename's detached-namespace EPERM behavior in both
  // directions: neither a stale source nor a stale destination may commit.
  errno = 0;
  expectFailure(renameat(detachedDirfd,
                         "rename-source",
                         AT_FDCWD,
                         AttachedRenameDestinationPath),
                EPERM);
  errno = 0;
  expectFailure(renameat(AT_FDCWD,
                         AttachedRenameSourcePath,
                         detachedDirfd,
                         "rename-destination"),
                EPERM);

  // fchdir intentionally permits a retained directory for existing I/O, but
  // wasmfs_unmount must not detach a nested mount through that stale CWD.
  int hostRootFd = open("/", O_RDONLY | O_DIRECTORY);
  assert(hostRootFd >= 0);
  assert(fchdir(detachedDirfd) == 0);
  assert(wasmfs_unmount("nested-mount") == -ENOENT);
  assert(fchdir(hostRootFd) == 0);
  assert(close(hostRootFd) == 0);

  assert(detachedDescendant->storedChild("victim") == detachedVictim);
  assert(detachedDescendant->storedChild("empty") == detachedEmpty);
  assert(detachedDescendant->storedChild("rename-source") ==
         detachedRenameSource);
  assert(!detachedDescendant->storedChild("created"));
  assert(!detachedDescendant->storedChild("created-directory"));
  assert(!detachedDescendant->storedChild("created-link"));
  assert(crossDestinationDirectory->storedChild("replacement") ==
         crossSource);
  assert(crossDestinationDirectory->storedChild("detached-descendant") ==
         detachedDescendant);
  assert(crossSource->locked().getParent() == crossDestinationDirectory);
  assert(detachedDescendant->locked().getParent() ==
         crossDestinationDirectory);
  assert(detachedVictim->locked().getParent() == detachedDescendant);
  assert(detachedEmpty->locked().getParent() == detachedDescendant);
  assert(detachedRenameSource->locked().getParent() == detachedDescendant);
  assert(nestedMount->locked().getParent() == detachedDescendant);
  assert(detachedDescendant->locked().getChild("nested-mount") == nestedMount);
  assert(access(AttachedRenameSourcePath, F_OK) == 0);
  assertMissing(AttachedRenameDestinationPath);
  assertSameMetadata(getMetadata(root), rootBeforeDetachAttempts);
  assertSameMetadata(getMetadata(crossDestinationDirectory),
                     crossDestinationBeforeDetachAttempts);
  assertSameMetadata(getMetadata(detachedDescendant),
                     detachedDescendantBeforeDetachAttempts);
  assertSameMetadata(getMetadata(detachedVictim),
                     detachedVictimBeforeDetachAttempts);
  assertSameMetadata(getMetadata(detachedEmpty),
                     detachedEmptyBeforeDetachAttempts);
  assertSameMetadata(getMetadata(detachedRenameSource),
                     detachedRenameSourceBeforeDetachAttempts);
  assert(backendState->state.transactionCalls ==
         detachedTransactionCallsBefore);
  assert(close(detachedDirfd) == 0);

  assertLegacyHooksUnused(backendState->state);
}

constexpr char MissingHookMountPath[] = "/wasmfs-atomic-namespace-missing";
constexpr char MissingHookCreatePath[] =
  "/wasmfs-atomic-namespace-missing/create";
constexpr char MissingHookDirectoryPath[] =
  "/wasmfs-atomic-namespace-missing/directory";
constexpr char MissingHookSymlinkPath[] =
  "/wasmfs-atomic-namespace-missing/link";
constexpr char MissingHookUnlinkPath[] =
  "/wasmfs-atomic-namespace-missing/unlink";
constexpr char MissingHookRmdirPath[] =
  "/wasmfs-atomic-namespace-missing/rmdir";
constexpr char MissingHookRenameSourcePath[] =
  "/wasmfs-atomic-namespace-missing/rename-source";
constexpr char MissingHookRenameDestinationPath[] =
  "/wasmfs-atomic-namespace-missing/rename-destination";

void testMissingNamespaceHookFailsClosed() {
  auto backend = std::make_unique<MissingAtomicNamespaceBackend>();
  auto* backendState = backend.get();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  assert(wasmfs_create_directory(MissingHookMountPath,
                                 0700,
                                 reinterpret_cast<::backend_t>(backendHandle)) ==
         0);
  auto root = backendState->root;
  assert(root);
  const auto rootBefore = getMetadata(root);

  // The default transaction hook returns ENOTSUP for every mutation kind. No
  // legacy directory callback may run as a fallback.
  errno = 0;
  expectFailure(open(MissingHookCreatePath, O_CREAT | O_EXCL | O_RDWR, 0600),
                ENOTSUP);
  assert(!root->storedChild("create"));
  assert(backendState->lastCreated);
  assert(!backendState->lastCreated->locked().getParent());

  errno = 0;
  expectFailure(mkdir(MissingHookDirectoryPath, 0700), ENOTSUP);
  assert(!root->storedChild("directory"));

  errno = 0;
  expectFailure(symlink("target", MissingHookSymlinkPath), ENOTSUP);
  assert(!root->storedChild("link"));

  auto unlinkFile = backendState->createFile(0600);
  root->seed("unlink", unlinkFile);
  errno = 0;
  expectFailure(unlink(MissingHookUnlinkPath), ENOTSUP);
  assert(root->storedChild("unlink") == unlinkFile);
  assert(unlinkFile->locked().getParent() == root);

  auto removeDirectory = backendState->createDirectory(0700);
  root->seed("rmdir", removeDirectory);
  errno = 0;
  expectFailure(rmdir(MissingHookRmdirPath), ENOTSUP);
  assert(root->storedChild("rmdir") == removeDirectory);
  assert(removeDirectory->locked().getParent() == root);

  auto renameSource = backendState->createFile(0600);
  auto renameReplacement = backendState->createFile(0600);
  root->seed("rename-source", renameSource);
  root->seed("rename-destination", renameReplacement);
  errno = 0;
  expectFailure(rename(MissingHookRenameSourcePath,
                       MissingHookRenameDestinationPath),
                ENOTSUP);
  assert(root->storedChild("rename-source") == renameSource);
  assert(root->storedChild("rename-destination") == renameReplacement);
  assert(renameSource->locked().getParent() == root);
  assert(renameReplacement->locked().getParent() == root);

  assertSameMetadata(getMetadata(root), rootBefore);
  assert(backendState->state.transactionCalls == 0);
  assert(backendState->state.commits == 0);
  assertLegacyHooksUnused(backendState->state);

  // An atomic namespace backend cannot be hidden behind the current virtual
  // case-folding wrapper, which cannot transform its complete request.
  errno = 0;
  assert(wasmfs_create_icase_backend(
           reinterpret_cast<::backend_t>(backendHandle)) == nullptr);
  assert(errno == ENOTSUP);

  assert(wasmfs_unmount(MissingHookMountPath) == 0);
}

} // anonymous namespace

int main() {
  testAtomicNamespaceMutations();
  testMissingNamespaceHookFailsClosed();
  puts("ok");
}
