// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <emscripten/wasmfs.h>

int main(void) {
  errno = 0;
  assert(wasmfs_create_opfs_backend_with_profile_lease("not/a-name") == NULL);
  assert(errno == EINVAL);

  errno = 0;
  assert(wasmfs_create_opfs_backend_with_profile_lease("no-pthreads") == NULL);
  assert(errno == ENOTSUP);
  puts("success");
  return 0;
}
