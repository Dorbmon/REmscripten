// Copyright 2022 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <string_view>

#include "file.h"
#include "paths.h"
#include "wasmfs.h"

namespace wasmfs::path {

namespace {

static inline constexpr size_t MAX_RECURSIONS = 40;

ParsedFile doParseFile(std::string_view path,
                       std::shared_ptr<Directory> base,
                       LinkBehavior links,
                       size_t& recursions);

ParsedFile getBaseDir(__wasi_fd_t basefd) {
  if (basefd == AT_FDCWD) {
    auto cwd = wasmFS.getCWD();
    if (int err = wasmFS.admitBackend(cwd->getBackend())) {
      return err;
    }
    return {std::move(cwd)};
  }
  auto openFile = wasmFS.getFileTable().locked().getEntry(basefd);
  if (!openFile) {
    return -EBADF;
  }
  auto file = openFile->locked().getFile();
  if (int err = wasmFS.admitBackend(file->getBackend())) {
    return err;
  }
  if (auto baseDir = file->dynCast<Directory>()) {
    return {baseDir};
  }
  return -ENOTDIR;
}

ParsedFile getChild(std::shared_ptr<Directory> dir,
                    std::string_view name,
                    LinkBehavior links,
                    size_t& recursions) {
  if (int err = wasmFS.admitBackend(dir->getBackend())) {
    return err;
  }
  auto child = dir->locked().getChildWithError(std::string(name));
  if (int err = child.getError()) {
    return err;
  }
  auto file = child.getFile();
  if (!file) {
    return -ENOENT;
  }
  if (int err = wasmFS.admitBackend(file->getBackend())) {
    return err;
  }
  if (links != NoFollowLinks) {
    while (auto link = file->dynCast<Symlink>()) {
      if (++recursions > MAX_RECURSIONS) {
        return -ELOOP;
      }
      auto target = link->getTarget();
      if (target.empty()) {
        return -ENOENT;
      }
      auto parsed = doParseFile(target, dir, FollowLinks, recursions);
      if (auto err = parsed.getError()) {
        return err;
      }
      file = parsed.getFile();
    }
  }
  return file;
}

ParsedParent doParseParent(std::string_view path,
                           std::shared_ptr<Directory> curr,
                           size_t& recursions) {
  // Empty paths never exist.
  if (path.empty()) {
    return {-ENOENT};
  }

  // Handle absolute paths.
  if (path.front() == '/') {
    curr = wasmFS.getRootDirectory();
    if (int err = wasmFS.admitBackend(curr->getBackend())) {
      return err;
    }
    path.remove_prefix(1);
  }

  // Ignore trailing '/'.
  while (!path.empty() && path.back() == '/') {
    path.remove_suffix(1);
  }

  // An empty path here means that the path was equivalent to "/" and does not
  // contain a child segment for us to return. The root is its own parent, so we
  // can handle this by returning (root, ".").
  if (path.empty()) {
    return {std::make_pair(std::move(curr), std::string_view("."))};
  }

  while (true) {
    // Skip any leading '/' for each segment.
    while (!path.empty() && path.front() == '/') {
      path.remove_prefix(1);
    }

    // If this is the leaf segment, return.
    size_t segment_end = path.find_first_of('/');
    if (segment_end == std::string_view::npos) {
      if (int err = wasmFS.admitBackend(curr->getBackend())) {
        return err;
      }
      return {std::make_pair(std::move(curr), path)};
    }

    // Try to descend into the child segment.
    // TODO: Check permissions on intermediate directories.
    auto segment = path.substr(0, segment_end);
    auto child = getChild(curr, segment, FollowLinks, recursions);
    if (auto err = child.getError()) {
      return err;
    }
    curr = child.getFile()->dynCast<Directory>();
    if (!curr) {
      return -ENOTDIR;
    }
    path.remove_prefix(segment_end);
  }
}

ParsedFile doParseFile(std::string_view path,
                       std::shared_ptr<Directory> base,
                       LinkBehavior links,
                       size_t& recursions) {
  auto parsed = doParseParent(path, base, recursions);
  if (auto err = parsed.getError()) {
    return {err};
  }
  auto& [parent, child] = parsed.getParentChild();
  return getChild(parent, child, links, recursions);
}

} // anonymous namespace

ParsedParent parseParent(std::string_view path, __wasi_fd_t basefd) {
  std::shared_ptr<Directory> baseDir;
  // POSIX absolute paths ignore dirfd/CWD. This matters after a scoped
  // profile drain seals the current directory: `/tmp` must remain reachable
  // even though a relative profile path is rejected.
  if (!path.empty() && path.front() == '/') {
    baseDir = wasmFS.getRootDirectory();
    if (int err = wasmFS.admitBackend(baseDir->getBackend())) {
      return err;
    }
  } else {
    auto base = getBaseDir(basefd);
    if (auto err = base.getError()) {
      return err;
    }
    baseDir = base.getFile()->cast<Directory>();
  }
  size_t recursions = 0;
  return doParseParent(path, baseDir, recursions);
}

ParsedFile
parseFile(std::string_view path, __wasi_fd_t basefd, LinkBehavior links) {
  std::shared_ptr<Directory> baseDir;
  // See parseParent(): an absolute pathname must not consult a sealed CWD or
  // otherwise invalid dirfd before switching to the root directory.
  if (!path.empty() && path.front() == '/') {
    baseDir = wasmFS.getRootDirectory();
    if (int err = wasmFS.admitBackend(baseDir->getBackend())) {
      return err;
    }
  } else {
    auto base = getBaseDir(basefd);
    if (auto err = base.getError()) {
      return err;
    }
    baseDir = base.getFile()->cast<Directory>();
  }
  size_t recursions = 0;
  return doParseFile(path, baseDir, links, recursions);
}

ParsedFile getFileAt(__wasi_fd_t fd, std::string_view path, int flags) {
  if ((flags & AT_EMPTY_PATH) && path.size() == 0) {
    // Don't parse a path, just use `dirfd` directly.
    if (fd == AT_FDCWD) {
      auto cwd = wasmFS.getCWD();
      if (int err = wasmFS.admitBackend(cwd->getBackend())) {
        return err;
      }
      return {std::move(cwd)};
    }
    auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
    if (!openFile) {
      return {-EBADF};
    }
    auto file = openFile->locked().getFile();
    if (int err = wasmFS.admitBackend(file->getBackend())) {
      return err;
    }
    return {std::move(file)};
  }
  auto links = (flags & AT_SYMLINK_NOFOLLOW) ? NoFollowLinks : FollowLinks;
  return path::parseFile(path, fd, links);
}

ParsedFile getFileFrom(std::shared_ptr<Directory> base, std::string_view path) {
  if (int err = wasmFS.admitBackend(base->getBackend())) {
    return err;
  }
  size_t recursions = 0;
  return doParseFile(path, base, FollowLinks, recursions);
}

} // namespace wasmfs::path
