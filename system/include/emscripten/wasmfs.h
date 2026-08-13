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

typedef struct Backend* backend_t;

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
// During an orderly `wasmfs_terminal_drain`, the lease is synchronously
// released only after all WasmFS terminal cleanup has succeeded. Ordinary
// whole-WasmFS/module teardown releases it only when no earlier terminal or
// ambiguous AccessHandle-close failure has occurred. `wasmfs_unmount` does not
// release it, because WasmFS retains created backends until teardown. There is
// intentionally no API to release it while the backend can still service
// filesystem operations. Abrupt execution-context termination relies on Web
// Locks' context cleanup and does not imply that open files were flushed.
backend_t wasmfs_create_opfs_backend_with_profile_lease(
  const char* profile_name);

// Creates a generic JSIMPL backend
backend_t wasmfs_create_jsimpl_backend(void);

backend_t wasmfs_create_icase_backend(backend_t backend);

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
// succeeded. A failed cleanup, or an ambiguous lease-release result, retains
// that backend's lease state and prevents a destructor retry; abrupt
// runtime/context termination can still cause browser Web Locks cleanup. For
// the supported leased-OPFS configuration, a zero result means its synchronous
// terminal lease release completed; a nonzero result retains the lease because
// cleanup or release was ambiguous or failed.
//
// Returns -EINVAL for a null result, -EDEADLK when called from an admitted
// WasmFS operation, -EAGAIN on the Emscripten runtime main thread, -EBUSY when
// another thread is draining, -ESHUTDOWN after a completed drain, or
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
