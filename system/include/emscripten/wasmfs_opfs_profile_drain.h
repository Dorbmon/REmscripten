/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Keep this header independently includable while sharing wasmfs.h's opaque
// backend handle declaration.
#ifndef EMSCRIPTEN_WASMFS_BACKEND_T_DEFINED
#define EMSCRIPTEN_WASMFS_BACKEND_T_DEFINED
typedef struct Backend* backend_t;
#endif

// The outcome of wasmfs_drain_opfs_profile_backend(). All error values are
// negative errno values. `error` is the first error observed. The descriptor
// count includes every target fd-table slot, including dup aliases and
// directory descriptors; `data_file_states` counts only final physical
// DataFile open-file states that needed flush/close.
//
// `backend_sealed` is set once the exact leased OPFS backend entered its
// one-way sealed state. `lease_released` is set only after the OPFS worker
// synchronously acknowledged Web Locks release. `backend_retired` is set only
// after that release and the dedicated OPFS worker has cleared its OPFS state,
// stopped its heartbeat, and been joined and quarantined from the pthread
// worker pool on the calling application pthread. A sealed backend remains
// unavailable after either success or failure.
//
// A failure without `lease_released` has no acknowledged safe release and must
// not be retried. In the exceptional case where a worker transaction is
// interrupted, physical Web Locks release can be indeterminate despite the
// absent acknowledgement. A failure after a successful Web Locks
// acknowledgement cannot reacquire that released lease, so it returns a
// nonzero error with `lease_released` set and `backend_retired` clear.
// Embedders that need the complete safe handoff must require a zero result and
// `backend_retired` rather than treating the narrower lease acknowledgement as
// success.
typedef struct wasmfs_opfs_profile_drain_result {
  int error;
  uint32_t detached_descriptors;
  uint32_t data_file_states;
  uint32_t libc_flush_failed;
  uint32_t data_flush_failures;
  uint32_t data_close_failures;
  uint32_t prior_close_failures;
  uint32_t lease_release_failures;
  uint32_t backend_retire_failures;
  uint8_t backend_sealed;
  uint8_t lease_released;
  uint8_t backend_retired;
} wasmfs_opfs_profile_drain_result;

// Atomically seal, flush, close, and release one backend created by either
// wasmfs_create_opfs_backend_with_profile_lease() or
// wasmfs_create_opfs_profile_namespace_backend(). This is an embedding
// primitive for a profile-specific orderly handoff. It leaves unrelated
// WasmFS mounts, descriptors, and stdio usable.
//
// The backend must be the exact live leased-OPFS backend returned by that
// factory. Default OPFS and every other backend return -ENOTSUP. The call must
// run on an application pthread, away from both Emscripten's runtime main
// thread and the browser main thread, and outside a public WasmFS operation.
// Once it has sealed the backend, all public accesses to it remain rejected
// even if cleanup fails. A zero result is a complete lease release and worker
// retirement; see the result flags for the exceptional post-release
// retirement-failure case. An active leased backend must use this primitive or
// wasmfs_terminal_drain before normal EXIT_RUNTIME; raw runtime/context teardown
// is not an orderly leased-profile handoff.
//
// The embedder must quiesce profile application work before calling this
// function. fflush(NULL) flushes buffers that existed at entry, but WasmFS
// cannot track libc FILE user-space buffers: after descriptor detachment, a
// later buffered fwrite() can accept bytes locally. Its later fd operation is
// not a safe fence: it can report EBADF or act on a reused descriptor number.
// During that flush, custom FILE callbacks must make no WasmFS call, including
// write(), because their raw fd write is indistinguishable from libc's stream
// write. This primitive is not a substitute for database or application
// shutdown coordination.
int wasmfs_drain_opfs_profile_backend(
  backend_t backend, wasmfs_opfs_profile_drain_result* result);

#ifdef __cplusplus
}
#endif
