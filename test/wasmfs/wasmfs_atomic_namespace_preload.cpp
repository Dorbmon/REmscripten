/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/wasmfs.h>

#include "../../system/lib/wasmfs/memory_backend.h"
#include "../../system/lib/wasmfs/wasmfs.h"

namespace {

struct PreloadState {
  int namespaceCommits = 0;
  int rawWriteCalls = 0;
  int pairedWriteCalls = 0;
  wasmfs::File::Metadata pairedMetadata = {};
};

class PreloadDataFile : public wasmfs::DataFile {
  PreloadState& state;
  std::vector<uint8_t> data;

  int open(wasmfs::oflags_t) override { return 0; }
  int close() override { return 0; }

  ssize_t write(const uint8_t*, size_t, off_t) override {
    ++state.rawWriteCalls;
    // A preload reaching this path would be a split mutation. Its caller used
    // to ignore the result, so the test also checks the counter in main().
    return -EIO;
  }

  ssize_t writeWithMetadata(const uint8_t* buffer,
                            size_t length,
                            off_t offset,
                            const Metadata& metadata) override {
    assert(offset == 0);
    ++state.pairedWriteCalls;
    state.pairedMetadata = metadata;
    data.assign(buffer, buffer + length);
    // The DataFile::Handle wrapper must restore the complete candidate after
    // the hook succeeds. Deliberately clobber the in-memory image here so the
    // assertion in main cannot pass merely because timestamps share a clock
    // tick.
    auto locked = this->locked();
    locked.setMTime(-123.0);
    locked.setCTime(-456.0);
    return length;
  }

  ssize_t read(uint8_t* buffer, size_t length, off_t offset) override {
    if (offset < 0 || static_cast<size_t>(offset) >= data.size()) {
      return 0;
    }
    auto available = data.size() - static_cast<size_t>(offset);
    auto count = std::min(length, available);
    memcpy(buffer, data.data() + offset, count);
    return count;
  }

  int setSize(off_t size) override {
    if (size < 0) {
      return -EINVAL;
    }
    data.resize(size);
    return 0;
  }

  off_t getSize() override { return data.size(); }
  int flush() override { return 0; }

public:
  PreloadDataFile(mode_t mode, wasmfs::backend_t backend, PreloadState& state)
    : DataFile(mode, backend), state(state) {}
};

class PreloadDirectory : public wasmfs::Directory {
  PreloadState& state;
  std::map<std::string, std::shared_ptr<wasmfs::File>> entries;

  std::shared_ptr<wasmfs::File> getChild(const std::string& name) override {
    auto entry = entries.find(name);
    return entry == entries.end() ? nullptr : entry->second;
  }

  std::shared_ptr<wasmfs::DataFile> insertDataFile(const std::string&,
                                                    mode_t) override {
    return nullptr;
  }

  std::shared_ptr<wasmfs::Directory> insertDirectory(const std::string&,
                                                      mode_t) override {
    return nullptr;
  }

  std::shared_ptr<wasmfs::Symlink> insertSymlink(const std::string&,
                                                  const std::string&) override {
    return nullptr;
  }

  int insertMove(const std::string&, std::shared_ptr<wasmfs::File>) override {
    return -EIO;
  }

  int removeChild(const std::string&) override { return -EIO; }

  int commitNamespaceMutation(const NamespaceMutation& mutation) override {
    assert(mutation.kind == NamespaceMutation::Kind::CreateDataFile);
    assert(mutation.destinationParent.get() == this);
    assert(mutation.subject);
    assert(mutation.destinationParentPostImage);
    assert(mutation.subjectPostImage);
    assert(!mutation.subject->locked().getParent());
    auto [_, inserted] =
      entries.insert({mutation.destinationName, mutation.subject});
    assert(inserted);
    ++state.namespaceCommits;
    return 0;
  }

  ssize_t getNumEntries() override { return entries.size(); }

  MaybeEntries getEntries() override {
    std::vector<Entry> result;
    for (const auto& [name, file] : entries) {
      result.push_back({name, file->kind, file->getIno()});
    }
    return {result};
  }

  std::string getName(std::shared_ptr<wasmfs::File> file) override {
    for (const auto& [name, entry] : entries) {
      if (entry == file) {
        return name;
      }
    }
    return "";
  }

  bool maintainsFileIdentity() override { return true; }

public:
  PreloadDirectory(mode_t mode,
                   wasmfs::backend_t backend,
                   PreloadState& state)
    : Directory(mode, backend), state(state) {}
};

class PreloadBackend : public wasmfs::Backend {
public:
  PreloadState state;
  std::shared_ptr<PreloadDirectory> root;

  bool requiresAtomicNamespaceMutations() const override { return true; }
  bool requiresAtomicMetadataMutations() const override { return true; }

  std::shared_ptr<wasmfs::DataFile> createFile(mode_t mode) override {
    return std::make_shared<PreloadDataFile>(mode, this, state);
  }

  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    auto result = std::make_shared<PreloadDirectory>(mode, this, state);
    if (!root) {
      root = result;
    }
    return result;
  }

  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string target) override {
    return std::make_shared<wasmfs::MemorySymlink>(target, this);
  }
};

std::unique_ptr<PreloadBackend> backend;

} // anonymous namespace

extern "C" void wasmfs_before_preload(void) {
  backend = std::make_unique<PreloadBackend>();
  auto handle = wasmfs::wasmFS.addBackend(std::move(backend));
  // Keep the state alive for assertions in main while the backend itself is
  // owned by WasmFS after registration.
  auto* preloadBackend = handle;
  assert(wasmfs_create_directory(
           "/wasmfs-atomic-preload",
           0700,
           reinterpret_cast<::backend_t>(preloadBackend)) == 0);
}

int main() {
  auto handle = wasmfs_get_backend_by_path("/wasmfs-atomic-preload/payload");
  assert(handle);
  auto* preloadBackend = reinterpret_cast<wasmfs::backend_t>(handle);
  auto* backendState = static_cast<PreloadBackend*>(preloadBackend);
  assert(backendState->root);
  assert(backendState->state.namespaceCommits == 1);
  assert(backendState->state.rawWriteCalls == 0);
  assert(backendState->state.pairedWriteCalls == 1);

  auto payload = backendState->root->locked().getChild("payload");
  assert(payload);
  assert(payload->locked().getMetadata().mtime ==
         backendState->state.pairedMetadata.mtime);
  assert(payload->locked().getMetadata().ctime ==
         backendState->state.pairedMetadata.ctime);
  puts("ok");
}
