// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the open file table.

#include "file_table.h"
#include "special_files.h"

#include <cstdlib>
#include <limits>

namespace wasmfs {

FileTable::FileTable() {
  entries.emplace_back();
  entryIncarnations.emplace_back();
  (void)OpenFileState::create(
    SpecialFiles::getStdin(), O_RDONLY, entries.back());
  entries.back()->uses = 1;
  entries.emplace_back();
  entryIncarnations.emplace_back();
  (void)OpenFileState::create(
    SpecialFiles::getStdout(), O_WRONLY, entries.back());
  entries.back()->uses = 1;
  entries.emplace_back();
  entryIncarnations.emplace_back();
  (void)OpenFileState::create(
    SpecialFiles::getStderr(), O_WRONLY, entries.back());
  entries.back()->uses = 1;
}

std::shared_ptr<OpenFileState> FileTable::Handle::getEntry(__wasi_fd_t fd) {
  if (fd >= fileTable.entries.size() || fd < 0) {
    return nullptr;
  }
  return fileTable.entries[fd];
}

FileTable::Handle::EntrySnapshot
FileTable::Handle::getEntrySnapshot(__wasi_fd_t fd) {
  if (fd >= fileTable.entries.size() || fd < 0) {
    return {};
  }
  return {fileTable.entries[fd], fileTable.entryIncarnations[fd]};
}

std::shared_ptr<DataFile>
FileTable::Handle::setEntry(__wasi_fd_t fd,
                            std::shared_ptr<OpenFileState> openFile) {
  assert(fd >= 0);
  assert(fd < WASMFS_FD_MAX);
  if (fd >= fileTable.entries.size()) {
    fileTable.entries.resize(fd + 1);
    fileTable.entryIncarnations.resize(fd + 1);
  }
  // Do not silently reuse a descriptor incarnation in a release build. There
  // is no error channel from setEntry(), and reusing it could let an ancient
  // fcntl snapshot pass its publication recheck, so terminate before changing
  // the table if a process somehow reaches this otherwise unreachable limit.
  if (fileTable.entryIncarnations[fd] ==
      std::numeric_limits<uint64_t>::max()) {
    std::abort();
  }
  if (openFile) {
    ++openFile->uses;
  }
  std::shared_ptr<DataFile> ret;
  if (auto oldOpenFile = fileTable.entries[fd]) {
    auto oldFile = oldOpenFile->locked().getFile();
    // POSIX process-owned locks are released when any descriptor referring to
    // the file is closed, not only when its final OpenFileState is closed.
    oldFile->locked().clearRecordLocks();
    if (--oldOpenFile->uses == 0) {
      ret = oldFile->dynCast<DataFile>();
    }
  }
  // In practice this counter cannot wrap: reaching it would require more
  // than 2^64 replacements of one descriptor without destroying WasmFS. The
  // pre-increment release-build guard above makes that limit fail closed.
  ++fileTable.entryIncarnations[fd];
  assert(fileTable.entryIncarnations[fd] != 0);
  fileTable.entries[fd] = openFile;
  return ret;
}

__wasi_fd_t
FileTable::Handle::addEntry(std::shared_ptr<OpenFileState> openFileState) {
  // TODO: add freelist to avoid linear lookup time.
  for (__wasi_fd_t i = 0; i < WASMFS_FD_MAX; i++) {
    if (!getEntry(i)) {
      (void)setEntry(i, openFileState);
      return i;
    }
  }
  return -EMFILE;
}

std::vector<std::shared_ptr<DataFile>> FileTable::Handle::detachAll() {
  std::vector<std::shared_ptr<DataFile>> closees;
  for (__wasi_fd_t fd = 0; fd < fileTable.entries.size(); ++fd) {
    if (auto closee = setEntry(fd, nullptr)) {
      closees.push_back(std::move(closee));
    }
  }
  return closees;
}

std::vector<std::shared_ptr<DataFile>>
FileTable::Handle::detachBackend(backend_t backend,
                                 uint32_t& detachedDescriptors) {
  std::vector<std::shared_ptr<DataFile>> closees;
  detachedDescriptors = 0;
  for (__wasi_fd_t fd = 0; fd < fileTable.entries.size(); ++fd) {
    auto openFile = getEntry(fd);
    if (!openFile) {
      continue;
    }
    if (openFile->locked().getFile()->getBackend() != backend) {
      continue;
    }
    ++detachedDescriptors;
    if (auto closee = setEntry(fd, nullptr)) {
      closees.push_back(std::move(closee));
    }
  }
  return closees;
}

int OpenFileState::create(std::shared_ptr<File> file,
                          oflags_t flags,
                          std::shared_ptr<OpenFileState>& out) {
  assert(file);
  std::vector<Directory::Entry> dirents;
  if (auto f = file->dynCast<DataFile>()) {
    if (int err = f->locked().open(flags & O_ACCMODE)) {
      return err;
    }
  } else if (auto d = file->dynCast<Directory>()) {
    // We are opening a directory; cache its contents for subsequent reads.
    auto lockedDir = d->locked();
    dirents = {{".", File::DirectoryKind, d->getIno()},
               {"..", File::DirectoryKind, lockedDir.getParent()->getIno()}};
    auto entries = lockedDir.getEntries();
    if (int err = entries.getError()) {
      return err;
    }
    dirents.insert(dirents.end(), entries->begin(), entries->end());
  }

  out = std::make_shared<OpenFileState>(
    private_key{0}, flags, file, std::move(dirents));
  return 0;
}

} // namespace wasmfs
