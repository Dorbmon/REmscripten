/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <stdio.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <utility>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

constexpr char MountPath[] = "/wasmfs-getdents-bounds";
constexpr off_t LargeDirectoryCookie = static_cast<off_t>(INT_MAX) + 1;
constexpr size_t TooLongNameLength =
  sizeof(((struct dirent*)nullptr)->d_name);
constexpr unsigned char Sentinel = 0xa5;

struct OneRecord {
  struct dirent entry;
  unsigned char trailing[16];
};

struct TwoRecords {
  struct dirent entries[2];
  unsigned char trailing[16];
};

class LongNameDirectory : public wasmfs::MemoryDirectory {
public:
  LongNameDirectory(mode_t mode, wasmfs::backend_t backend)
    : MemoryDirectory(mode, backend) {}

protected:
  wasmfs::Directory::MaybeEntries getEntries() override {
    auto entries = MemoryDirectory::getEntries();
    assert(!entries.getError());
    entries->push_back({std::string(TooLongNameLength, 'x'),
                        wasmfs::File::DataFileKind,
                        0});
    return entries;
  }
};

class LongNameBackend : public wasmfs::Backend {
public:
  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<wasmfs::MemoryDataFile>(mode, this);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<LongNameDirectory>(mode, this);
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(target, this);
  }
};

template <typename T>
void assertUnchanged(const T& record) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
  for (size_t i = 0; i < sizeof(record); ++i) {
    assert(bytes[i] == Sentinel);
  }
}

void testGetdentsBounds() {
  auto backend = std::make_unique<LongNameBackend>();
  auto backendHandle = wasmfs::wasmFS.addBackend(std::move(backend));
  auto publicBackend = reinterpret_cast<::backend_t>(backendHandle);
  assert(wasmfs_create_directory(MountPath, 0700, publicBackend) == 0);

  int fd = open(MountPath, O_RDONLY | O_DIRECTORY);
  assert(fd >= 0);

  // A directory cookie beyond the snapshot is EOF, not an opportunity to
  // narrow the off_t position to int and corrupt the descriptor state.
  assert(lseek(fd, LargeDirectoryCookie, SEEK_SET) == LargeDirectoryCookie);
  OneRecord record;
  memset(&record, Sentinel, sizeof(record));
  assert(getdents(fd, &record.entry, sizeof(record.entry)) == 0);
  assertUnchanged(record);
  assert(lseek(fd, 0, SEEK_CUR) == LargeDirectoryCookie);

  // Check that a too-long name after a valid entry does not result in a
  // partial output buffer or directory-position update.
  assert(lseek(fd, 1, SEEK_SET) == 1);
  TwoRecords records;
  memset(&records, Sentinel, sizeof(records));
  errno = 0;
  assert(getdents(fd, records.entries, sizeof(records.entries)) == -1);
  assert(errno == ENAMETOOLONG);
  assertUnchanged(records);
  assert(lseek(fd, 0, SEEK_CUR) == 1);

  // The snapshot begins with "." and "..". Seek to the injected entry and
  // verify that a name too large for dirent::d_name is rejected before the
  // output buffer or position can change.
  assert(lseek(fd, 2, SEEK_SET) == 2);
  memset(&record, Sentinel, sizeof(record));
  errno = 0;
  assert(getdents(fd, &record.entry, sizeof(record.entry)) == -1);
  assert(errno == ENAMETOOLONG);
  assertUnchanged(record);
  assert(lseek(fd, 0, SEEK_CUR) == 2);

  assert(close(fd) == 0);
  assert(wasmfs_unmount(MountPath) == 0);
}

} // anonymous namespace

int main() {
  testGetdentsBounds();
  puts("ok");
  return 0;
}
