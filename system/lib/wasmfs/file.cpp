// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the file object.

#include "file.h"
#include "wasmfs.h"
#include "wasmfs_internal.h"
#include <algorithm>
#include <emscripten/threading.h>
#include <fcntl.h>

namespace wasmfs {

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

void DataFile::Handle::preloadFromJS(int index) {
  // TODO: Each Datafile type could have its own impl of file preloading.
  // Create a buffer with the required file size.
  std::vector<uint8_t> buffer(_wasmfs_get_preloaded_file_size(index));

  // Ensure that files are preloaded from the main thread.
  assert(emscripten_is_main_runtime_thread());

  // Load data into the in-memory buffer.
  _wasmfs_copy_preloaded_file_data(index, buffer.data());

  write((const uint8_t*)buffer.data(), buffer.size(), 0);
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

int Directory::Handle::removeChild(const std::string& name) {
  auto& dcache = getDir()->dcache;
  auto entry = dcache.find(name);
  // If this is a mount, we don't need to call into the backend.
  if (entry != dcache.end() && entry->second.kind == DCacheKind::Mount) {
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
