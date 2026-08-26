// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the file object.

#include "file.h"
#include "backend.h"
#include "wasmfs.h"
#include "wasmfs_internal.h"
#include <algorithm>
#include <emscripten/threading.h>
#include <fcntl.h>

namespace wasmfs {

namespace {

bool requiresAtomicNamespaceMutations(Directory* directory) {
  if (auto* backend = directory->getBackend()) {
    return backend->requiresAtomicNamespaceMutations();
  }
  return false;
}

// A namespace-create factory is allowed to allocate a private candidate, but
// must not return an object belonging to another storage domain. The caller
// takes the candidate's long-lived File lock immediately afterward to check
// that it has never been linked before the durable transaction.
bool hasNamespaceCreateCandidateBackend(
  const std::shared_ptr<File>& candidate, backend_t backend) {
  return candidate && candidate->getBackend() == backend;
}

File::Metadata namespaceMutationPostImage(File::Handle& locked, double now) {
  auto metadata = locked.getMetadata();
  metadata.mtime = now;
  metadata.ctime = now;
  return metadata;
}

File::Metadata namespaceCTimePostImage(File::Handle& locked, double now) {
  auto metadata = locked.getMetadata();
  // Namespace operations that change a link or parent update ctime but not
  // mtime. Keep the complete image so a namespace backend does not need to
  // merge a delta.
  metadata.ctime = now;
  return metadata;
}

int checkedNamespaceMutationResult(int result) {
  // Transaction hooks use the same zero-or-negative errno ABI as the other
  // WasmFS backend hooks. A malformed positive result must not become a
  // successful namespace operation with unknown persistent state.
  return result > 0 ? -EIO : result;
}

} // anonymous namespace

void File::Handle::applyRecordLock(short type,
                                   off_t start,
                                   std::optional<off_t> end) {
  RecordLock lock = {type, start, end};
  auto rangesOverlap = [](const RecordLock& lhs, const RecordLock& rhs) {
    return (!lhs.end || rhs.start < *lhs.end) &&
           (!rhs.end || lhs.start < *rhs.end);
  };
  auto endsBeforeOrAt = [](const std::optional<off_t>& lhs, off_t rhs) {
    return lhs && *lhs <= rhs;
  };
  auto endsAfter = [](const std::optional<off_t>& lhs,
                      const std::optional<off_t>& rhs) {
    if (!lhs) {
      return rhs.has_value();
    }
    return rhs && *lhs > *rhs;
  };

  auto& locks = file->recordLocks;
  std::vector<RecordLock> updated;
  updated.reserve(locks.size() + (lock.type != F_UNLCK));

  for (const auto& current : locks) {
    if (!rangesOverlap(current, lock)) {
      updated.push_back(current);
      continue;
    }

    // Preserve the current lock's non-overlapping left and right portions.
    if (current.start < lock.start) {
      updated.push_back({current.type, current.start, lock.start});
    }
    if (lock.end && !endsBeforeOrAt(current.end, *lock.end)) {
      updated.push_back({current.type, *lock.end, current.end});
    }
  }

  if (lock.type != F_UNLCK) {
    updated.push_back(lock);
  }

  std::sort(updated.begin(), updated.end(), [](const RecordLock& lhs,
                                                 const RecordLock& rhs) {
    return lhs.start < rhs.start;
  });

  locks.clear();
  locks.reserve(updated.size());
  for (const auto& current : updated) {
    if (!locks.empty() && locks.back().type == current.type &&
        (!locks.back().end || current.start <= *locks.back().end)) {
      if (endsAfter(current.end, locks.back().end)) {
        locks.back().end = current.end;
      }
      continue;
    }
    locks.push_back(current);
  }
}

//
// DataFile
//

int DataFile::Handle::preloadFromJS(int index) {
  // TODO: Each Datafile type could have its own impl of file preloading.
  // Create a buffer with the required file size.
  std::vector<uint8_t> buffer(_wasmfs_get_preloaded_file_size(index));

  // Ensure that files are preloaded from the main thread.
  assert(emscripten_is_main_runtime_thread());

  // Load data into the in-memory buffer.
  _wasmfs_copy_preloaded_file_data(index, buffer.data());

  // A backend that requires paired data/metadata writes must not receive this
  // startup population through the legacy raw write path. Treat a failed or
  // short preload as fatal to initialization so an atomic namespace entry is
  // never followed by a falsely successful, split content update.
  if (auto* backend = getFile()->getBackend();
      backend && backend->requiresAtomicMetadataMutations()) {
    if (buffer.empty()) {
      return 0;
    }
    auto metadata = getMetadata();
    const double now = emscripten_date_now();
    metadata.mtime = now;
    metadata.ctime = now;
    auto result = writeWithMetadata(
      reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size(), 0, metadata);
    if (result < 0) {
      return result;
    }
    return static_cast<size_t>(result) == buffer.size() ? 0 : -EIO;
  }

  // Preserve legacy preload behavior for non-atomic backends, including its
  // historical best-effort raw write semantics.
  write((const uint8_t*)buffer.data(), buffer.size(), 0);
  return 0;
}

//
// Directory
//

void Directory::Handle::cacheChild(const std::string& name,
                                   std::shared_ptr<File> child,
                                   DCacheKind kind) {
  // Update the dcache if the backend hasn't opted out of using the dcache or if
  // this is a mount point, in which case it is not under the control of the
  // backend.
  if (kind == DCacheKind::Mount || !getDir()->maintainsFileIdentity()) {
    auto& dcache = getDir()->dcache;
    [[maybe_unused]] auto [_, inserted] = dcache.insert({name, {kind, child}});
    assert(inserted && "inserted child already existed!");
  }
  // Set the child's parent.
  assert(child->locked().getParent() == nullptr ||
         child->locked().getParent() == getDir());
  child->locked().setParent(getDir());
}

std::shared_ptr<File> Directory::Handle::getChild(const std::string& name) {
  auto child = getChildWithError(name);
  if (child.getError()) {
    return nullptr;
  }
  return child.getFile();
}

Directory::MaybeFile
Directory::Handle::getChildWithError(const std::string& name) {
  // Unlinked directories must be empty, without even "." or ".."
  if (!getParent()) {
    return std::shared_ptr<File>();
  }
  if (name == ".") {
    return file;
  }
  if (name == "..") {
    return getParent();
  }
  // Check whether the cache already contains this child.
  auto& dcache = getDir()->dcache;
  if (auto it = dcache.find(name); it != dcache.end()) {
    return it->second.file;
  }
  // Otherwise check whether the backend contains an underlying file we don't
  // know about.
  auto child = getDir()->getChildWithError(name);
  if (child.getError()) {
    return child;
  }
  auto file = child.getFile();
  if (!file) {
    return child;
  }
  cacheChild(name, file, DCacheKind::Normal);
  return child;
}

bool Directory::Handle::mountChild(const std::string& name,
                                   std::shared_ptr<File> child) {
  assert(child);
  // Cannot insert into an unlinked directory.
  if (!getParent()) {
    return false;
  }
  cacheChild(name, child, DCacheKind::Mount);
  return true;
}

bool Directory::Handle::isMountChild(const std::string& name) {
  auto& dcache = getDir()->dcache;
  auto entry = dcache.find(name);
  return entry != dcache.end() && entry->second.kind == DCacheKind::Mount;
}

std::shared_ptr<DataFile>
Directory::Handle::insertDataFile(const std::string& name, mode_t mode) {
  // Cannot insert into an unlinked directory.
  if (!getParent()) {
    return nullptr;
  }
  auto child = getDir()->insertDataFile(name, mode);
  if (!child) {
    return nullptr;
  }
  cacheChild(name, child, DCacheKind::Normal);
  updateMTime();
  return child;
}

std::shared_ptr<Directory>
Directory::Handle::insertDirectory(const std::string& name, mode_t mode) {
  // Cannot insert into an unlinked directory.
  if (!getParent()) {
    return nullptr;
  }
  auto child = getDir()->insertDirectory(name, mode);
  if (!child) {
    return nullptr;
  }
  cacheChild(name, child, DCacheKind::Normal);
  updateMTime();
  return child;
}

std::shared_ptr<Symlink>
Directory::Handle::insertSymlink(const std::string& name,
                                 const std::string& target) {
  // Cannot insert into an unlinked directory.
  if (!getParent()) {
    return nullptr;
  }
  auto child = getDir()->insertSymlink(name, target);
  if (!child) {
    return nullptr;
  }
  cacheChild(name, child, DCacheKind::Normal);
  updateMTime();
  return child;
}

int Directory::Handle::insertDataFileWithNamespaceTransaction(
  const std::string& name,
  mode_t mode,
  std::shared_ptr<DataFile>& result) {
  result = nullptr;
  if (!requiresAtomicNamespaceMutations(getDir().get())) {
    result = insertDataFile(name, mode);
    return result ? 0 : -EIO;
  }

  if (!getParent()) {
    return -ENOENT;
  }
  auto* backend = getDir()->getBackend();
  if (!backend) {
    return -EIO;
  }
  auto child = backend->createFile(mode);
  if (!child) {
    return -EIO;
  }
  if (!hasNamespaceCreateCandidateBackend(child, backend)) {
    return -EIO;
  }
  // Keep the private candidate locked from its metadata snapshot through the
  // durable transaction and post-commit publication. Otherwise a backend-side
  // actor could update it between the snapshot and the image published below.
  auto lockedChild = child->locked();
  if (lockedChild.getParent()) {
    return -EIO;
  }

  const double now = emscripten_date_now();
  NamespaceMutation mutation;
  mutation.kind = NamespaceMutation::Kind::CreateDataFile;
  mutation.destinationParent = getDir();
  mutation.destinationName = name;
  mutation.subject = child;
  mutation.destinationParentPostImage =
    namespaceMutationPostImage(*this, now);
  mutation.subjectPostImage = lockedChild.getMetadata();

  if (int error = checkedNamespaceMutationResult(
        getDir()->commitNamespaceMutation(mutation))) {
    return error;
  }

  // The child had no path or parent before the durable transaction. Publish
  // its complete candidate image, reachability, and parent metadata only
  // after that transaction. In particular, do not rely on the constructor's
  // currently matching image: a transaction hook may require this explicit
  // publication contract.
  lockedChild.publishMetadataAfterAtomicMutation(*mutation.subjectPostImage);
  cacheChild(name, child, DCacheKind::Normal);
  publishMetadata(*mutation.destinationParentPostImage);
  result = std::move(child);
  return 0;
}

int Directory::Handle::insertDirectoryWithNamespaceTransaction(
  const std::string& name,
  mode_t mode,
  std::shared_ptr<Directory>& result) {
  result = nullptr;
  if (!requiresAtomicNamespaceMutations(getDir().get())) {
    result = insertDirectory(name, mode);
    return result ? 0 : -EIO;
  }

  if (!getParent()) {
    return -ENOENT;
  }
  auto* backend = getDir()->getBackend();
  if (!backend) {
    return -EIO;
  }
  auto child = backend->createDirectory(mode);
  if (!child) {
    return -EIO;
  }
  if (!hasNamespaceCreateCandidateBackend(child, backend)) {
    return -EIO;
  }
  // Keep the private candidate locked from its metadata snapshot through the
  // durable transaction and post-commit publication.
  auto lockedChild = child->locked();
  if (lockedChild.getParent()) {
    return -EIO;
  }

  const double now = emscripten_date_now();
  NamespaceMutation mutation;
  mutation.kind = NamespaceMutation::Kind::CreateDirectory;
  mutation.destinationParent = getDir();
  mutation.destinationName = name;
  mutation.subject = child;
  mutation.destinationParentPostImage =
    namespaceMutationPostImage(*this, now);
  mutation.subjectPostImage = lockedChild.getMetadata();

  if (int error = checkedNamespaceMutationResult(
        getDir()->commitNamespaceMutation(mutation))) {
    return error;
  }

  lockedChild.publishMetadataAfterAtomicMutation(*mutation.subjectPostImage);
  cacheChild(name, child, DCacheKind::Normal);
  publishMetadata(*mutation.destinationParentPostImage);
  result = std::move(child);
  return 0;
}

int Directory::Handle::insertSymlinkWithNamespaceTransaction(
  const std::string& name,
  const std::string& target,
  std::shared_ptr<Symlink>& result) {
  result = nullptr;
  if (!requiresAtomicNamespaceMutations(getDir().get())) {
    result = insertSymlink(name, target);
    return result ? 0 : -EIO;
  }

  if (!getParent()) {
    return -ENOENT;
  }
  auto* backend = getDir()->getBackend();
  if (!backend) {
    return -EIO;
  }
  auto child = backend->createSymlink(target);
  if (!child) {
    return -EIO;
  }
  if (!hasNamespaceCreateCandidateBackend(child, backend)) {
    return -EIO;
  }
  // Keep the private candidate locked from its metadata snapshot through the
  // durable transaction and post-commit publication.
  auto lockedChild = child->locked();
  if (lockedChild.getParent()) {
    return -EIO;
  }

  const double now = emscripten_date_now();
  NamespaceMutation mutation;
  mutation.kind = NamespaceMutation::Kind::CreateSymlink;
  mutation.destinationParent = getDir();
  mutation.destinationName = name;
  mutation.subject = child;
  mutation.destinationParentPostImage =
    namespaceMutationPostImage(*this, now);
  mutation.subjectPostImage = lockedChild.getMetadata();

  if (int error = checkedNamespaceMutationResult(
        getDir()->commitNamespaceMutation(mutation))) {
    return error;
  }

  lockedChild.publishMetadataAfterAtomicMutation(*mutation.subjectPostImage);
  cacheChild(name, child, DCacheKind::Normal);
  publishMetadata(*mutation.destinationParentPostImage);
  result = std::move(child);
  return 0;
}

// TODO: consider moving this to be `Backend::move` to avoid asymmetry between
// the source and destination directories and/or taking `Directory::Handle`
// arguments to prove that the directories have already been locked.
int Directory::Handle::insertMove(const std::string& name,
                                  std::shared_ptr<File> file) {
  // Cannot insert into an unlinked directory.
  if (!getParent()) {
    return -EPERM;
  }

  // Look up the file in its old parent's cache.
  auto oldParent = file->locked().getParent();
  auto& oldCache = oldParent->dcache;
  auto oldIt = std::find_if(oldCache.begin(), oldCache.end(), [&](auto& kv) {
    return kv.second.file == file;
  });

  // TODO: Handle moving mount points correctly by only updating caches without
  // involving the backend.

  // Attempt the move.
  if (auto err = getDir()->insertMove(name, file)) {
    return err;
  }

  if (oldIt != oldCache.end()) {
    // Do the move and update the caches.
    auto [oldName, entry] = *oldIt;
    assert(oldName.size());
    // Update parent pointers and caches to reflect the successful move.
    oldCache.erase(oldIt);
    auto& newCache = getDir()->dcache;
    auto [it, inserted] = newCache.insert({name, entry});
    if (!inserted) {
      // Update and overwrite the overwritten file.
      it->second.file->locked().setParent(nullptr);
      it->second = entry;
    }
  } else {
    // This backend doesn't use the dcache.
    assert(getDir()->maintainsFileIdentity());
  }

  file->locked().setParent(getDir());

  // TODO: Moving mount points probably shouldn't update the mtime.
  oldParent->locked().updateMTime();
  updateMTime();

  return 0;
}

int Directory::Handle::insertMoveWithNamespaceTransaction(
  Directory::Handle& oldParent,
  const std::string& oldName,
  const std::string& newName,
  std::shared_ptr<File> file,
  std::shared_ptr<File> replacement) {
  if (!requiresAtomicNamespaceMutations(getDir().get())) {
    return insertMove(newName, std::move(file));
  }

  // Match insertMove(): a retained descriptor can name an unlinked directory,
  // but it cannot be used as a durable rename destination.
  if (!getParent()) {
    return -EPERM;
  }

  auto oldDirectory = oldParent.unlocked()->cast<Directory>();
  auto newDirectory = getDir();
  if (oldDirectory->getBackend() != newDirectory->getBackend()) {
    return -EXDEV;
  }
  // Keep the moved inode locked from validation through durable commit and
  // post-commit publication. Otherwise a concurrent metadata mutation could
  // commit after we snapshot the rename ctime image and then be overwritten by
  // that stale image when the rename publishes.
  auto lockedFile = file->locked();
  if (lockedFile.getParent() != oldDirectory) {
    return -EIO;
  }
  std::optional<File::Handle> lockedReplacement;
  if (replacement) {
    lockedReplacement.emplace(replacement);
    if (lockedReplacement->getParent() != newDirectory) {
      return -EIO;
    }
  }

  // Validate all cache observations before the durable transaction. A cache
  // mismatch after commit would leave no safe way to report an error without
  // also withholding a necessary in-memory publication.
  Directory::DCacheEntry cachedEntry = {DCacheKind::Normal, file};
  auto& oldCache = oldDirectory->dcache;
  auto oldEntry = oldCache.find(oldName);
  if (!oldDirectory->maintainsFileIdentity()) {
    if (oldEntry == oldCache.end() || oldEntry->second.file != file) {
      return -EIO;
    }
    cachedEntry = oldEntry->second;
  }

  auto& newCache = newDirectory->dcache;
  auto newEntry = newCache.find(newName);
  if (!newDirectory->maintainsFileIdentity()) {
    if (replacement) {
      if (newEntry == newCache.end() || newEntry->second.file != replacement) {
        return -EIO;
      }
    } else if (newEntry != newCache.end()) {
      return -EIO;
    }
  }

  const double now = emscripten_date_now();
  NamespaceMutation mutation;
  mutation.kind = NamespaceMutation::Kind::Rename;
  mutation.sourceParent = oldDirectory;
  mutation.sourceName = oldName;
  mutation.destinationParent = newDirectory;
  mutation.destinationName = newName;
  mutation.subject = file;
  mutation.replacement = replacement;
  mutation.sourceParentPostImage =
    namespaceMutationPostImage(oldParent, now);
  mutation.subjectPostImage = namespaceCTimePostImage(lockedFile, now);
  if (lockedReplacement) {
    mutation.replacementPostImage =
      namespaceCTimePostImage(*lockedReplacement, now);
  }
  if (oldDirectory != newDirectory) {
    mutation.destinationParentPostImage =
      namespaceMutationPostImage(*this, now);
  }

  if (int error = checkedNamespaceMutationResult(
        getDir()->commitNamespaceMutation(mutation))) {
    return error;
  }

  if (!oldDirectory->maintainsFileIdentity()) {
    oldCache.erase(oldEntry);
  }
  if (!newDirectory->maintainsFileIdentity()) {
    newEntry = newCache.find(newName);
    if (newEntry != newCache.end()) {
      newCache.erase(newEntry);
    }
    [[maybe_unused]] auto [_, inserted] =
      newCache.insert({newName, cachedEntry});
    assert(inserted);
  }
  if (lockedReplacement) {
    lockedReplacement->publishMetadataAfterAtomicMutation(
      *mutation.replacementPostImage);
    lockedReplacement->setParent(nullptr);
  }
  lockedFile.publishMetadataAfterAtomicMutation(*mutation.subjectPostImage);
  lockedFile.setParent(newDirectory);
  oldParent.publishMetadata(*mutation.sourceParentPostImage);
  if (mutation.destinationParentPostImage) {
    publishMetadata(*mutation.destinationParentPostImage);
  }
  return 0;
}

int Directory::Handle::removeChild(const std::string& name) {
  auto& dcache = getDir()->dcache;
  auto entry = dcache.find(name);
  // If this is a mount, we don't need to call into the backend.
  if (entry != dcache.end() && entry->second.kind == DCacheKind::Mount) {
    // Mounts live only in the dcache, so removing the cache publication also
    // unlinks the mounted root. Otherwise an fd retained across unmount could
    // still look linked and accept operations into a detached namespace.
    entry->second.file->locked().setParent(nullptr);
    dcache.erase(entry);
    return 0;
  }
  if (auto err = getDir()->removeChild(name)) {
    assert(err < 0);
    return err;
  }
  if (entry != dcache.end()) {
    entry->second.file->locked().setParent(nullptr);
    dcache.erase(entry);
  }
  updateMTime();
  return 0;
}

int Directory::Handle::removeChildWithNamespaceTransaction(
  const std::string& name,
  std::shared_ptr<File> child,
  NamespaceMutation::Kind kind) {
  if (!requiresAtomicNamespaceMutations(getDir().get()) ||
      isMountChild(name)) {
    return removeChild(name);
  }

  // Keep the unlinked object locked through the durable transaction and its
  // ctime publication. A retained descriptor may otherwise commit newer
  // metadata between the snapshot and the post-commit image.
  auto lockedChild = child->locked();
  if (lockedChild.getParent() != getDir()) {
    return -EIO;
  }
  auto& dcache = getDir()->dcache;
  auto entry = dcache.find(name);
  if (!getDir()->maintainsFileIdentity() &&
      (entry == dcache.end() || entry->second.file != child)) {
    return -EIO;
  }

  const double now = emscripten_date_now();
  NamespaceMutation mutation;
  mutation.kind = kind;
  mutation.sourceParent = getDir();
  mutation.sourceName = name;
  mutation.subject = child;
  mutation.sourceParentPostImage = namespaceMutationPostImage(*this, now);
  mutation.subjectPostImage = namespaceCTimePostImage(lockedChild, now);

  if (int error = checkedNamespaceMutationResult(
        getDir()->commitNamespaceMutation(mutation))) {
    return error;
  }

  lockedChild.publishMetadataAfterAtomicMutation(*mutation.subjectPostImage);
  lockedChild.setParent(nullptr);
  if (entry != dcache.end()) {
    dcache.erase(entry);
  }
  publishMetadata(*mutation.sourceParentPostImage);
  return 0;
}

std::string Directory::Handle::getName(std::shared_ptr<File> file) {
  if (getDir()->maintainsFileIdentity()) {
    return getDir()->getName(file);
  }
  auto& dcache = getDir()->dcache;
  for (auto it = dcache.begin(); it != dcache.end(); ++it) {
    if (it->second.file == file) {
      return it->first;
    }
  }
  return "";
}

ssize_t Directory::Handle::getNumEntries() {
  size_t mounts = 0;
  auto& dcache = getDir()->dcache;
  for (auto it = dcache.begin(); it != dcache.end(); ++it) {
    if (it->second.kind == DCacheKind::Mount) {
      ++mounts;
    }
  }
  auto numReal = getDir()->getNumEntries();
  if (numReal < 0) {
    return numReal;
  }
  return numReal + mounts;
}

Directory::MaybeEntries Directory::Handle::getEntries() {
  auto entries = getDir()->getEntries();
  if (entries.getError()) {
    return entries;
  }
  auto& dcache = getDir()->dcache;
  for (auto it = dcache.begin(); it != dcache.end(); ++it) {
    auto& [name, entry] = *it;
    if (entry.kind == DCacheKind::Mount) {
      entries->push_back({name, entry.file->kind, entry.file->getIno()});
    }
  }
  return entries;
}

} // namespace wasmfs
