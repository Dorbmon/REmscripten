// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This file defines the modular backend abstract class.

#pragma once

#include <cstddef>
#include <cstdint>
#include <errno.h>

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

  // Construct the root supplied by wasmfs_create_directory() when its
  // explicit backend differs from the parent mount. Most backends use the
  // same factory for an ordinary child and a mount root, so retain that
  // historical default. A persistent backend which returns private directory
  // candidates from createDirectory() for atomic namespace transactions must
  // override this: an explicit second mount must never publish an unreachable
  // candidate and report success.
  virtual std::shared_ptr<Directory> createMountRootDirectory(mode_t mode) {
    return createDirectory(mode);
  }

  // Whether this backend permits WasmFS to attempt explicit POSIX mode and
  // timestamp setters on its Files. A true result does not promise that the
  // metadata is durable or survives a remount: each File may still reject its
  // candidate metadata image from File::persistMetadata(). Backends that
  // cannot represent these setters must override this to avoid falsely
  // reporting a successful logical mutation.
  virtual bool supportsExplicitMetadataMutation() const { return true; }

  // A backend that persists a file's contents together with its implicit
  // timestamp metadata must opt into the paired DataFile mutation hooks. Once
  // this is true, WasmFS never falls back to write() or setSize() for a data
  // mutation that changes content or length: a missing hook is an explicit
  // ENOTSUP instead of a successful, split data/metadata update.
  //
  // This is deliberately narrower than supportsExplicitMetadataMutation().
  // The latter covers chmod and utimens-style metadata-only operations;
  // this capability covers the metadata post-image of write and resize
  // transactions.
  virtual bool requiresAtomicMetadataMutations() const { return false; }

  // A backend that persists namespace changes together with directory and
  // child metadata must opt into Directory::commitNamespaceMutation(). Once
  // this is true, WasmFS never falls back to the legacy insert/remove/move
  // hooks for a persistent namespace mutation. A missing transaction hook is
  // an explicit ENOTSUP rather than a split durable namespace and in-memory
  // cache update.
  //
  // For a create request, createFile/createDirectory/createSymlink must return
  // a private, unreachable candidate owned by this Backend and with no parent
  // link. It must not make that candidate durably reachable before
  // Directory::commitNamespaceMutation() returns success. If the transaction
  // returns a negative errno, the backend must roll back or reclaim every
  // temporary allocation and leave none of that request durable. The only
  // exception is a backend that has latched its storage domain unrecoverable;
  // it must then fail every later access explicitly rather than expose an
  // ambiguous partial namespace.
  //
  // This is intentionally independent of requiresAtomicMetadataMutations().
  // A backend may need one transaction shape for file data and another for
  // directory topology, and opting into one must not imply the other.
  // No production backend enables this generic seam yet; profile-backed OPFS
  // integration must first supply the complete durable transaction contract.
  virtual bool requiresAtomicNamespaceMutations() const { return false; }

  // Whether this backend provides a storage domain in which WasmFS can safely
  // implement POSIX process-owned record locks. Returning true is not a
  // promise that arbitrary external writers participate in locking. A
  // persistent backend must instead establish an exclusive external owner
  // before opting in, so a successful fcntl lock never merely masks
  // cross-instance data races.
  virtual bool supportsRecordLocks() const { return false; }

  // Whether this backend can provide the sharing and lifetime semantics of a
  // file-backed, non-writable MAP_SHARED mapping. Backends that store file
  // data as snapshots or copy-on-write generations must return false rather
  // than let WasmFS expose a detached heap copy as a shared mapping. The
  // default preserves the historical behavior for existing backends.
  virtual bool supportsReadOnlySharedMmap() const { return true; }

  // Validate storage state once after a public WasmFS operation has acquired
  // this backend's admission token and before pathname or descriptor caches
  // can answer from a previously discovered File object. Persistent backends
  // use this to fail closed when a selected durable generation no longer
  // validates; the default leaves historical in-memory and non-persistent
  // backends unchanged. The implementation must return zero or a negative
  // errno and must not re-enter a public WasmFS operation.
  virtual int validateOperation() { return 0; }

  // A leased OPFS backend uses this narrow operation admission protocol while
  // a profile-specific drain seals it. Other backends remain no-ops so their
  // normal filesystem traffic is unaffected by the scoped drain.
  virtual int acquireProfileOperation() { return 0; }
  virtual void releaseProfileOperation() {}

  // These hooks are intentionally implemented only by the explicit leased
  // OPFS profile factories. The scoped drain never treats an arbitrary
  // persistent backend as safe to hand off.
  virtual bool isLeasedOPFSProfileBackend() const { return false; }
  virtual int beginOPFSProfileDrain() { return -ENOTSUP; }
  // Preflight the browser-owned resources that must be quiescent before a
  // successful leased profile handoff releases its Web Lock. A failure leaves
  // the lease owned and lets the caller report a normal drain failure.
  virtual int prepareOPFSProfileRetirement(bool check_resources) {
    return -ENOTSUP;
  }
  // `lease_released` distinguishes a Web Locks acknowledgement from a later
  // worker-context retirement error in the same scoped transaction.
  virtual int finishOPFSProfileDrain(bool success, bool* lease_released) {
    if (lease_released) {
      *lease_released = false;
    }
    return -ENOTSUP;
  }
  // Retire the dedicated OPFS worker after finishOPFSProfileDrain(true) has
  // acknowledged release. This is intentionally separate from lease release:
  // a post-release failure must remain observable rather than being confused
  // with a retained lease or silently reported as a safe handoff.
  virtual int retireOPFSProfileBackend(bool transaction_succeeded) {
    return -ENOTSUP;
  }
  virtual int getOPFSProfilePriorCloseError() const { return 0; }

  // Experimental, deliberately narrow control-plane interface for the
  // profile-log V2 recovery primitive.  This is not a general filesystem
  // operation: the only payload is one opaque root value, and non-V2
  // backends reject it explicitly.  Keeping the dispatch here lets the C ABI
  // validate an opaque backend pointer through WasmFS before it reaches a
  // backend-specific implementation.
  virtual int readOPFSProfileLogV2Root(uint64_t* value) {
    return -ENOTSUP;
  }
  virtual int commitOPFSProfileLogV2Root(uint64_t value) {
    return -ENOTSUP;
  }

  // Experimental V4 manifest-store interface. This is deliberately narrower
  // than a filesystem: it proves the variable-length immutable manifest and
  // selector protocol that a later logical-inode backend will use. Keeping
  // the dispatch here lets its C ABI validate an opaque backend pointer
  // through WasmFS before it reaches the profile-specific implementation.
  // Non-V4 backends reject both calls explicitly.
  virtual int readOPFSProfileLogV4Manifest(uint8_t*,
                                           size_t,
                                           size_t*) {
    return -ENOTSUP;
  }
  virtual int commitOPFSProfileLogV4Manifest(const uint8_t*, size_t) {
    return -ENOTSUP;
  }

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
