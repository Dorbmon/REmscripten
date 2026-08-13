/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#pragma once

#include <stdint.h>

// The outcome of wasmfs_terminal_drain(). All error values are negative errno
// values. `error` is the first error observed while draining. The individual
// counters record every failed data-file operation and every failed backend
// terminal finalizer. A backend that retains a terminal resource because an
// earlier cleanup already failed does not count as a failed finalizer.
//
// `data_file_states` counts physical DataFile open-file states, not descriptor
// aliases. Directory states are detached but are not DataFiles and are not
// counted. For example, dup() aliases one data-file state and is counted once,
// while two independent open() calls for the same file are counted twice.
typedef struct wasmfs_terminal_drain_result {
  int error;
  uint32_t data_file_states;
  // `fflush(NULL)` exposes one aggregate failure result, not a count of
  // individual FILE failures. This field is consequently always zero or one.
  uint32_t libc_flush_failed;
  uint32_t data_flush_failures;
  uint32_t data_close_failures;
  uint32_t backend_terminal_failures;
} wasmfs_terminal_drain_result;
