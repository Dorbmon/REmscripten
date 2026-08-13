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
#include <condition_variable>
#include <emscripten/html5.h>
#include <emscripten/wasmfs_terminal_drain.h>
#include <mutex>
#include <sys/stat.h>
#include <vector>
#include <wasi/api.h>

namespace wasmfs {

class WasmFS {

  std::vector<std::unique_ptr<Backend>> backendTable;
  // A single WasmFS instance may own at most one cooperative terminal lease.
  // This is reserved before an OPFS factory creates its ProxyWorker or asks
  // the browser for a Web Lock, then retained for the instance lifetime.
  bool terminalLeaseOwnerReserved = false;
  FileTable fileTable;
  std::mutex mutex;
  std::mutex cwdTransitionMutex;

  enum class TerminalState { Running, Draining, Drained, Failed };
  std::mutex operationMutex;
  std::condition_variable operationCV;
  size_t activeOperations = 0;
  TerminalState terminalState = TerminalState::Running;

  static thread_local WasmFS* activeOperationWasmFS;
  static thread_local size_t activeOperationDepth;
  static thread_local WasmFS* terminalStdioFlushWasmFS;

  std::shared_ptr<Directory> rootDirectory;
  std::shared_ptr<Directory> cwd;

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
  // Holds admission to a public WasmFS operation. Admission is reentrant on
  // one thread so wrappers can delegate to other WasmFS entrypoints without a
  // terminal drain splitting one logical operation. Direct use of WasmFS's
  // internal C++ implementation interfaces remains unsupported by the public
  // terminal-drain contract.
  class Operation {
    WasmFS* wasmfs = nullptr;
    bool admitted = false;
    bool tracksDepth = false;
    bool ownsActiveOperation = false;
    int error = 0;

  public:
    enum class Kind { General, StdioFlushWrite };

    explicit Operation(WasmFS& wasmfs, Kind kind = Kind::General);
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    ~Operation();

    explicit operator bool() const { return admitted; }
    int getError() const { return error; }
  };

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

  // See wasmfs_terminal_drain().
  int terminalDrain(wasmfs_terminal_drain_result* result);

  FileTable& getFileTable() { return fileTable; }

  std::shared_ptr<Directory> getRootDirectory() { return rootDirectory; };

  std::shared_ptr<Directory> getCWD() {
    const std::lock_guard<std::mutex> lock(mutex);
    return cwd;
  };

  CWDTransition beginCWDTransition() { return CWDTransition(*this); }

  backend_t addBackend(std::unique_ptr<Backend> backend);

  // Reserve the one cooperative terminal lease owner before constructing a
  // backend with external browser state. Returns false when an owner already
  // exists. A factory must cancel a reservation when its acquisition fails.
  bool reserveTerminalLeaseOwner();
  void cancelTerminalLeaseOwnerReservation();
};

// Global state instance.
extern WasmFS wasmFS;

} // namespace wasmfs
