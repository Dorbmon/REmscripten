/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void expectUnsupportedOpen(const char* path, int flag) {
  errno = 0;
  assert(open(path, O_CREAT | O_EXCL | O_RDWR | flag, 0600) == -1);
  assert(errno == ENOTSUP);

  // Rejecting a synchronization mode must not leave an O_CREAT side effect.
  errno = 0;
  assert(access(path, F_OK) == -1);
  assert(errno == ENOENT);
}

int main() {
  expectUnsupportedOpen("sync-open", O_SYNC);
  expectUnsupportedOpen("dsync-open", O_DSYNC);
#ifdef O_RSYNC
  expectUnsupportedOpen("rsync-open", O_RSYNC);
#endif
  puts("ok");
  return 0;
}
