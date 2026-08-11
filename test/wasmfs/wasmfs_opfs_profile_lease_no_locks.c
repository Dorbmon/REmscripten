// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <emscripten/wasmfs.h>

static void ExpectInvalidProfileName(const char* name) {
  errno = 0;
  assert(wasmfs_create_opfs_backend_with_profile_lease(name) == NULL);
  assert(errno == EINVAL);
}

int main(void) {
  ExpectInvalidProfileName(NULL);
  ExpectInvalidProfileName("");
  ExpectInvalidProfileName("not/a-profile-name");

  char max_length[129];
  memset(max_length, 'a', sizeof(max_length) - 1);
  max_length[sizeof(max_length) - 1] = '\0';
  errno = 0;
  assert(wasmfs_create_opfs_backend_with_profile_lease(max_length) == NULL);
  assert(errno == ENOSYS);

  char too_long[130];
  memset(too_long, 'a', sizeof(too_long) - 1);
  too_long[sizeof(too_long) - 1] = '\0';
  ExpectInvalidProfileName(too_long);

  // This must fail before the OPFS backend is registered, rather than waiting
  // for a lock or failing later during mount.
  errno = 0;
  assert(wasmfs_create_opfs_backend_with_profile_lease("node-test") == NULL);
  assert(errno == ENOSYS);

  puts("success");
  return 0;
}
