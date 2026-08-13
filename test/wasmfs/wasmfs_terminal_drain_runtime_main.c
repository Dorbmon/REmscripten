/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

int main(void) {
  // This test intentionally does not use PROXY_TO_PTHREAD. Rejecting the
  // runtime thread must happen before terminal state changes, so an ordinary
  // filesystem operation immediately afterward still works.
  assert(emscripten_is_main_runtime_thread());

  wasmfs_terminal_drain_result result = {};
  assert(wasmfs_terminal_drain(&result) == -EAGAIN);

  int fd = open("runtime-main-still-running", O_CREAT | O_RDWR, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);

  printf("success\n");
  return 0;
}
