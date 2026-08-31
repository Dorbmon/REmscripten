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

// The outcome of wasmfs_drain_opfs_profile_backend() or
// wasmfs_fail_closed_opfs_profile_backend(). All error values are negative
// errno values. `error` is the first error observed. The descriptor count
// includes every target fd-table slot, including dup aliases and directory
// descriptors; `data_file_states` counts only final physical DataFile
// open-file states that needed flush/close.
//
// `backend_sealed` is set once the exact leased OPFS backend entered its
// one-way sealed state. `lease_released` is set only after the OPFS worker
// synchronously acknowledged Web Locks release. `backend_retired` is set only
// after that release and the dedicated OPFS worker has cleared its OPFS state,
// stopped its heartbeat, and been joined and quarantined from the pthread
// worker pool on the calling application pthread. A sealed backend remains
// unavailable after either success or failure. An intentional fail-closed
// retirement returns -ESHUTDOWN after clean local cleanup and must leave both
// `lease_released` and `backend_retired` clear; its retained lease is not an
// orderly handoff.
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
// wasmfs_create_opfs_profile_namespace_backend(), or the experimental
// wasmfs_create_opfs_profile_log_v2_control_backend(), or the experimental
// wasmfs_create_opfs_profile_log_v3_data_backend(), or the experimental
// wasmfs_create_opfs_profile_log_v4_manifest_backend(), or the experimental
// wasmfs_create_opfs_profile_log_v4_filesystem_backend(). This is an embedding
// primitive for a profile-specific orderly handoff. The V2 control and V4
// manifest primitives have no mount or fd-table descriptors of their own,
// while V3 has at most its one explicitly attached DataFile projection and V4
// filesystem descriptors are detached and closed by the drain; all still own
// private OPFS handles and the same cooperative lease. The drain leaves
// unrelated WasmFS mounts, descriptors, and stdio usable.
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

// Intentionally fail-close a profile handoff after the embedding application
// has quiesced its profile work but cannot certify a clean profile result. It
// accepts the same exact leased backend and application-pthread-only calling
// context as wasmfs_drain_opfs_profile_backend(). It seals that backend,
// attempts to flush and close its detached descriptors, and runs backend
// retirement preparation with resource-success checks disabled so private
// browser-owned OPFS handles cannot reach raw runtime destruction still open.
//
// This is not a release operation and never reports a durable handoff. After
// it successfully seals the backend, it returns -ESHUTDOWN if local cleanup
// has no more specific error, retains the Web Lock, abandons the dedicated
// OPFS worker, and leaves `lease_released` and `backend_retired` clear. A
// later scoped or terminal drain cannot retry that one-way failure state. If
// validation or sealing cannot begin, it returns the corresponding error
// instead. Callers must inspect the result and preserve their higher-level
// failure; this API only makes the native failure teardown explicit and
// destructor-safe.
//
// Like the successful drain, callers must quiesce profile application work
// first. It is not a substitute for database or application shutdown
// coordination, and it intentionally makes no persistence, recovery, or lock
// release claim.
int wasmfs_fail_closed_opfs_profile_backend(
  backend_t backend, wasmfs_opfs_profile_drain_result* result);

// Test-only controls for the selected V4 immutable-log acknowledgement-loss
// witness. These symbols are present only in an artifact linked with
// WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE. They are not a
// general OPFS failure-injection interface.
//
// `wasmfs_opfs_profile_log_v4_test_proxy_completion_arm()` grants exactly one
// pending fault to the calling application pthread and returns one on success.
// Only that thread may consume the fault, which prevents unrelated profile
// work on another thread from taking it before the selected operation reaches
// the V4 post-flush, pre-publication boundary. It returns zero if the selected
// module has already armed or consumed the one-shot control.
int wasmfs_opfs_profile_log_v4_test_proxy_completion_arm(void);

// These counters are meaningful only for the same selected V4 test artifact.
// The latch count reaches one when the post-flush fault was consumed;
// `proxies_after_latch` is an immediate diagnostic snapshot, not a lifetime
// guarantee about later runtime teardown traffic.
int wasmfs_opfs_profile_log_v4_test_proxy_completion_latch_count(void);
int wasmfs_opfs_profile_log_v4_test_proxies_after_latch(void);

#ifdef __cplusplus
}
#endif
