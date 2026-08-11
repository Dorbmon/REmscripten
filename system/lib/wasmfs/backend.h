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

  virtual ~Backend() = default;
};

typedef backend_t (*backend_constructor_t)(void*);
} // namespace wasmfs
