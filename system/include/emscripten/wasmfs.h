/*
 * Copyright 2021 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#pragma once

#include <stdint.h>
#include <sys/stat.h>
#include <emscripten/wasmfs_terminal_drain.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EMSCRIPTEN_WASMFS_BACKEND_T_DEFINED
#define EMSCRIPTEN_WASMFS_BACKEND_T_DEFINED
typedef struct Backend* backend_t;
#endif

// Obtains the backend_t of a specified path.
backend_t wasmfs_get_backend_by_path(const char* _Nonnull path);

// Obtains the backend_t of a specified fd.
backend_t wasmfs_get_backend_by_fd(int fd);

// Creates and opens a new file using a specific backend.
// Returns the file descriptor for the new file like `open`. Returns a negative
// value on error. TODO: It might be worth returning a more specialized type
// like __wasi_fd_t here.
// TODO: Remove this function so that only directories can be mounted.
int wasmfs_create_file(const char* _Nonnull pathname, mode_t mode, backend_t backend);

// Creates a new directory using a specific backend.
// Returns 0 on success like `mkdir`, or a negative value on error.
// TODO: Add an alias with wasmfs_mount.
int wasmfs_create_directory(const char* _Nonnull path, mode_t mode, backend_t backend);

// Unmounts the directory (Which must be a valid mountpoint) at a specific path.
// Returns 0 on success, or a negative value on error.
int wasmfs_unmount(const char* _Nonnull path);

// Backend creation

// Creates a new JSFile Backend
backend_t wasmfs_create_js_file_backend(void);

// A function that receives a void* and returns a backend.
typedef backend_t (*backend_constructor_t)(void*);

backend_t wasmfs_create_memory_backend(void);

// Fetch backend
//
// Creates a new fetchfs backend.  FetchFS will backstop filesystem
// reads to HTTP fetch requests, which will download just specific
// ranges of the requested files.  FetchFS works best when your web
// server supports HTTP range requests, and it's important that those
// files are not stored encrypted or compressed at rest.  FetchFS by
// default will dispatch HTTP requests to URLs beginning with base_url
// and ending with whatever the file's path is relative to where the
// fetchfs directory is mounted.
//
// Individual range requests will be no bigger than chunk_size, and will
// be aligned to boundaries of chunk_size.  Files smaller than chunk_size
// will be downloaded all at once.
//
// If chunk_size is 0, a reasonable default value will be used.
//
// Note: this cannot be called on the browser main thread because it might
// deadlock while waiting for its dedicated worker thread to be spawned.
//
// Note: This function blocks on the main browser thread returning to its event
// loop. Calling this function while holding a lock the main thread is waiting
// to acquire will cause a deadlock.
//
// TODO: Add an async version of this function that will work on the main
// thread.
//
backend_t wasmfs_create_fetch_backend(const char* _Nonnull base_url, uint32_t chunk_size);

backend_t wasmfs_create_node_backend(const char* _Nonnull root);

// Note: this cannot be called on the browser main thread because it might
// deadlock while waiting for the OPFS dedicated worker thread to be spawned.
//
// Note: This function blocks on the main browser thread returning to its event
// loop. Calling this function while holding a lock the main thread is waiting
// to acquire will cause a deadlock.
//
// TODO: Add an async version of this function that will work on the main
// thread.
backend_t wasmfs_create_opfs_backend(void);

// Creates an OPFS backend that holds an exclusive, storage-bucket-scoped
// profile lease for the lifetime of the WasmFS instance. This is intended to
// coordinate independent Wasm modules that mount the same persistent profile;
// it is not a replacement for POSIX record locks or database locking.
//
// Web Locks are cooperative. Every profile writer that needs this protection
// must opt into this API with the same canonical `profile_name`. A default
// OPFS backend or another same-storage-bucket writer can ignore the lease, so
// this API does not establish physical ownership of OPFS data.
// Only one leased OPFS backend may be created in a WasmFS instance; a second
// attempt fails with EBUSY before it creates a worker or requests Web Locks.
//
// `profile_name` must be a non-empty ASCII identifier of at most 128
// characters containing only letters, digits, '.', '-', and '_'. The lease is
// acquired by the OPFS backend's dedicated worker before it requests the OPFS
// root directory or any OPFS handle. A malformed name returns NULL and EINVAL.
// This API otherwise requires pthreads and returns NULL and ENOTSUP when built
// without them. With pthreads, it returns NULL and ENOSYS when Web Locks are
// unavailable, EBUSY when another module sharing the storage bucket already
// holds the lease, or EIO for an unexpected Web Locks failure.
//
// An active leased backend must complete the result-bearing
// `wasmfs_terminal_drain` or `wasmfs_drain_opfs_profile_backend` on an
// application pthread before normal EXIT_RUNTIME. `wasmfs_unmount` does not
// release it, because WasmFS retains created backends until teardown. There is
// intentionally no API to release it while the backend can still service
// filesystem operations. Raw EXIT_RUNTIME without one of those explicit
// drains is not a supported orderly leased-profile lifecycle: browser-context
// Web Locks cleanup may occur, but establishes neither durability nor worker
// retirement evidence.
backend_t wasmfs_create_opfs_backend_with_profile_lease(
  const char* profile_name);

// Creates an opt-in leased OPFS backend whose visible filesystem namespace is
// stored in one opaque container file: an append-only payload log with two
// alternating mutable, checksummed selector blocks. A second permanent,
// checksummed PREPARED/PUBLISHED journal records whether that container may be
// reset as an unexposed bootstrap or is an established profile identity.
// Unlike the direct OPFS backend, this backend can make a populated directory
// rename a single committed namespace-root transition and can flush that
// namespace root from a directory descriptor. It is intended for a
// profile-specific embedding that requires those semantics; it does not alter
// the behavior or capabilities of wasmfs_create_opfs_backend().
//
// The container is write-through in this initial implementation: a successful
// namespace mutation has already written and flushed a new complete namespace
// generation before it returns. First mount writes and flushes the caller's
// root mode before flushing PUBLISHED and exposing the root. This trades
// performance and container growth for a narrow, fail-closed recovery
// boundary. It currently supports regular files and directories only.
// Symlinks, explicit POSIX metadata setters, and persistence of WasmFS's
// implicit atime/mtime/ctime updates remain unsupported; a successful
// directory fsync is therefore not a claim of complete POSIX metadata
// durability. Callers must still use
// wasmfs_drain_opfs_profile_backend (or wasmfs_terminal_drain) for the leased
// backend's orderly handoff. One namespace backend supports one logical mount
// for its lifetime; a caller must not alias or remount it in the same WasmFS
// instance, because WasmFS File identity carries append and record-lock state.
// Complete the drain and create a fresh backend instead.
//
// The same profile-name validation, pthread requirement, cooperative Web Lock
// ownership, and one-leased-backend-per-WasmFS-instance rules as
// wasmfs_create_opfs_backend_with_profile_lease() apply. On failure returns
// NULL and sets errno. A caller must not treat this experimental backend as a
// complete Chromium profile implementation without separately proving its
// higher-level service shutdown and database semantics.
backend_t wasmfs_create_opfs_profile_namespace_backend(
  const char* profile_name);

// Creates the experimental V2 profile-log recovery control primitive.  It is
// intentionally *not* a filesystem backend: callers can only read or replace
// one unsigned 64-bit logical-root value through the two functions below.
//
// The primitive keeps its bootstrap, two fixed root images, and control
// records in opaque regular OPFS files. A replacement writes and flushes an
// inactive root image and duplicated descriptor first, then flushes two CLEAN
// phase witnesses. A fresh factory chooses the old root while exactly one
// phase witness names the new generation and chooses the new root only after
// both do. It fails closed when the selected phase or descriptor quorum is
// malformed; it does not use OPFS directory rename or directory fsync.
// A fresh instance that observes the valid one-witness state is read-only:
// read_root returns the old root, while commit_root returns -ESHUTDOWN until a
// future protocol defines a separately durable repair decision.
//
// This is a focused protocol experiment, not a Chromium profile backend. It
// has no directory tree, database, metadata, crash, or physical directory
// durability claim. It follows the leased-OPFS factory's profile-name,
// pthread, cooperative Web Lock, and explicit-drain requirements. On failure
// it returns NULL and sets errno.
backend_t wasmfs_create_opfs_profile_log_v2_control_backend(
  const char* profile_name);

// Read or replace the V2 primitive's opaque logical-root value. Both return
// zero on success or a negative errno. `backend` must be the exact V2 control
// backend returned by the factory above; foreign and ordinary OPFS backends
// fail with -ENOTSUP. These calls participate in WasmFS operation admission,
// so they fail closed after a scoped or terminal drain.
int wasmfs_opfs_profile_log_v2_read_root(backend_t backend,
                                         uint64_t* _Nonnull value);
int wasmfs_opfs_profile_log_v2_commit_root(backend_t backend,
                                           uint64_t value);

// Creates the experimental V3 fixed-file payload projection. This is
// deliberately not a mountable filesystem backend: it creates no directories
// and exposes at most one regular DataFile through wasmfs_create_file() in the
// caller's existing WasmFS namespace. The caller must pass the same
// |payload_mode| when attaching that projection in a fresh WasmFS instance.
//
// V3 stores one bounded payload and its complete mode/atime/mtime/ctime image
// in immutable, checksummed pages in two opaque OPFS arena files. Its fixed
// bootstrap/control files select only a CLEAN descriptor quorum. The payload
// file opts into WasmFS's paired data/metadata hooks, so successful write,
// resize, and explicit metadata operations have already committed their full
// post-image before WasmFS publishes it in memory. A fresh instance selects
// the old image for a valid g/g+1 phase split and then rejects mutation with
// ESHUTDOWN; malformed selected control or page data fails the factory.
//
// This is a focused COW data-transaction experiment, not a Chromium profile
// backend. It has no persistent namespace, directory fsync/rename, symlink,
// record-lock, database, quota-management, physical-crash, or Chrome-service
// claim. It follows the same pthread-only leased-OPFS and explicit-drain
// requirements as the V2 control primitive. On failure it returns NULL and
// sets errno.
backend_t wasmfs_create_opfs_profile_log_v3_data_backend(
  const char* profile_name,
  mode_t payload_mode);

// Creates a generic JSIMPL backend
backend_t wasmfs_create_jsimpl_backend(void);

backend_t wasmfs_create_icase_backend(backend_t backend);

#include <emscripten/wasmfs_opfs_profile_drain.h>

// Similar to fflush(0), but also flushes all internal buffers inside WasmFS.
// This is necessary because in a Web environment we must buffer at an
// additional level after libc, since console.log() prints entire lines, that
// is, we can't print individual characters as libc feeds them to us, so we
// buffer them and call console.log() only after a newline. This function will
// actually flush all buffers and add newlines as necessary to get everything
// printed out.
void wasmfs_flush(void);

// Permanently stop public WasmFS filesystem operations and drain the currently
// open file states. This is an embedding primitive for orderly teardown, not a
// substitute for application-level quiescence. The embedding must stop
// higher-level work before calling it: this function waits for already-admitted
// WasmFS operations and can wait indefinitely for a blocking operation.
//
// This function must run away from Emscripten's runtime main thread, normally
// on the application pthread. In browsers that thread services the JavaScript
// event loop; in Node it is also the default runtime thread. Once draining
// begins, direct negative-errno APIs and backend factories fail with ESHUTDOWN.
// POSIX wrappers that route through WasmFS's WASI fd ABI (such as read, write,
// and close), and raw WASI fd entrypoints, expose the valid ABI equivalent
// ECANCELED/CANCELED; other POSIX wrappers such as open or fstat retain their
// direct ESHUTDOWN error. The void wasmfs_flush() reports rejected admission in
// errno. The drain then performs the aggregate libc `fflush(NULL)`, atomically
// detaches every descriptor alias, and attempts both flush() and close() on
// every detached DataFile state. Directory states are detached but are not
// flushed or closed. It continues after failures and returns the
// first negative errno in both its return value and result->error; the counters
// in `result` record the observable failures. A failure is terminal: the
// descriptors stay detached, new operations remain rejected, and a later call
// returns -ESHUTDOWN rather than retrying cleanup.
//
// While `fflush(NULL)` is running, the drain admits fd writes on its own thread
// so ordinary libc streams can flush buffered data. The WasmFS fd ABI cannot
// distinguish a direct write made by a custom FILE callback from libc's own
// stream write. Therefore an embedding calling this function must ensure every
// custom FILE callback makes no WasmFS call, including write(); calls to other
// public WasmFS APIs are rejected after the terminal transition.
//
// This function does not establish database recovery, record-lock, metadata,
// or application task quiescence guarantees. In particular, it is not a
// Chromium profile shutdown protocol. A leased OPFS backend synchronously
// releases its cooperative lease only after all earlier terminal cleanup has
// succeeded. A failed cleanup, or an ambiguous lease-release result, prevents
// a destructor retry; abrupt runtime/context termination can still cause
// browser Web Locks cleanup. For the supported leased-OPFS configuration, a
// zero result means synchronous terminal lease release and worker retirement
// completed. A nonzero result is not a successful handoff: it can retain the
// lease, or it can follow an acknowledged lease release whose worker
// retirement did not complete.
//
// Returns -EINVAL for a null result, -EDEADLK when called from an admitted
// WasmFS operation, -EAGAIN on either the Emscripten runtime main thread or
// browser main thread, -EBUSY when another thread is draining, -ESHUTDOWN
// after a completed drain, or
// result->error after this call's cleanup attempt.
int wasmfs_terminal_drain(wasmfs_terminal_drain_result* _Nonnull result);

// Hooks

// A hook users can do to create the root directory. Overriding this allows the
// user to set a particular backend as the root. If this is not set then the
// default backend is used.
backend_t wasmfs_create_root_dir(void);

// A hook users can do to run code during WasmFS startup. This hook happens
// before file preloading, so user code could create backends and mount them,
// which would then affect in which backend the preloaded files are loaded (the
// preloaded files have paths, and so they are added to that path and whichever
// backend is present there).
void wasmfs_before_preload(void);

#ifdef __cplusplus
}
#endif
