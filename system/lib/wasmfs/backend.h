// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the modular backend abstract class.

#pragma once

#include "file.h"

namespace wasmfs {
// A backend (or modular backend) provides a base to extend WasmFS with new
// storage capabilities. Files and directories will be represented in the file
// system structure, but their underlying backing could exist in persistent
// storage, another thread, etc.
class Backend {

public:
  virtual std::shared_ptr<DataFile> createFile(mode_t mode) = 0;
  virtual std::shared_ptr<Directory> createDirectory(mode_t mode) = 0;
  virtual std::shared_ptr<Symlink> createSymlink(std::string target) = 0;

  // Whether this backend permits WasmFS to mutate a File's logical mode and
  // timestamps in response to explicit POSIX metadata setters. A true result
  // permits only the current in-memory WasmFS mutation; it does not promise
  // that the metadata is durably stored or survives a remount or reload.
  // Backends that cannot represent these setters must override this to avoid
  // falsely reporting a successful logical mutation.
  virtual bool supportsExplicitMetadataMutation() const { return true; }

  // Whether this backend provides a storage domain in which WasmFS can safely
  // implement POSIX process-owned record locks. Returning true is not a
  // promise that arbitrary external writers participate in locking. A
  // persistent backend must instead establish an exclusive external owner
  // before opting in, so a successful fcntl lock never merely masks
  // cross-instance data races.
  virtual bool supportsRecordLocks() const { return false; }

  // Called after WasmFS has permanently drained its public descriptor table.
  // Backends must not begin new filesystem work from this hook. A false
  // argument means that some earlier cleanup failed; a backend holding a
  // cooperative lease must retain it in that case. A true argument permits a
  // backend to release a terminal resource and must return a negative errno if
  // that release has an ambiguous or failed outcome. The default is a no-op.
  virtual int terminalDrainFinished(bool success) { return 0; }

  // Lease-owning backends run after ordinary backends. This prevents an
  // ordinary terminal-resource failure from releasing a cooperative profile
  // lease. WasmFS enforces one cooperative terminal lease owner per instance.
  virtual bool releasesTerminalLease() const { return false; }

  virtual ~Backend() = default;
};

typedef backend_t (*backend_constructor_t)(void*);
} // namespace wasmfs
