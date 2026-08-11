// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the global state.

#pragma once

#include "backend.h"
#include "file.h"
#include "file_table.h"
#include <assert.h>
#include <emscripten/html5.h>
#include <mutex>
#include <sys/stat.h>
#include <vector>
#include <wasi/api.h>

namespace wasmfs {

class WasmFS {

  std::vector<std::unique_ptr<Backend>> backendTable;
  FileTable fileTable;
  std::shared_ptr<Directory> rootDirectory;
  std::shared_ptr<Directory> cwd;
  std::mutex mutex;
  std::mutex cwdTransitionMutex;

  void setCWD(std::shared_ptr<Directory> directory) {
    const std::lock_guard<std::mutex> lock(mutex);
    cwd = directory;
  };

  // Private method to initialize root directory once.
  // Initializes default directories including dev/stdin, dev/stdout,
  // dev/stderr. Refers to the same std streams in the open file table.
  std::shared_ptr<Directory> initRootDirectory();

  // Initialize files specified by --preload-file option.
  void preloadFiles();

public:
  // Serializes CWD installation with mount detachment. Acquire this before
  // renameMutex, `mutex`, FileTable, and any File or Directory lock.
  class CWDTransition {
    friend class WasmFS;

    WasmFS& wasmfs;
    std::unique_lock<std::mutex> lock;

    explicit CWDTransition(WasmFS& wasmfs)
      : wasmfs(wasmfs), lock(wasmfs.cwdTransitionMutex) {}

  public:
    CWDTransition(const CWDTransition&) = delete;
    CWDTransition& operator=(const CWDTransition&) = delete;
    CWDTransition(CWDTransition&&) = default;
    CWDTransition& operator=(CWDTransition&&) = delete;

    std::shared_ptr<Directory> getCWD() {
      assert(lock.owns_lock());
      return wasmfs.getCWD();
    }

    void setCWD(std::shared_ptr<Directory> directory) {
      assert(lock.owns_lock());
      wasmfs.setCWD(std::move(directory));
    }
  };

  WasmFS();
  ~WasmFS();

  FileTable& getFileTable() { return fileTable; }

  std::shared_ptr<Directory> getRootDirectory() { return rootDirectory; };

  std::shared_ptr<Directory> getCWD() {
    const std::lock_guard<std::mutex> lock(mutex);
    return cwd;
  };

  CWDTransition beginCWDTransition() { return CWDTransition(*this); }

  backend_t addBackend(std::unique_ptr<Backend> backend) {
    const std::lock_guard<std::mutex> lock(mutex);
    backendTable.push_back(std::move(backend));
    return backendTable.back().get();
  }
};

// Global state instance.
extern WasmFS wasmFS;

} // namespace wasmfs
