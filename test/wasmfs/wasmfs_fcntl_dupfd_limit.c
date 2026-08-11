// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

// WASMFS_FD_MAX is an internal WasmFS implementation limit. Keep this test
// tied to that fixed table size without exposing it as a public API contract.
static const int kLastWasmfsDescriptor = 4095;
static const int kFirstInvalidWasmfsDescriptor = 4096;
static const char kPath[] = "wasmfs-fcntl-dupfd-limit";
static const char kMarker = 'x';

int main(void) {
  int source = open(kPath, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(source >= 3 && source < kLastWasmfsDescriptor);
  assert(write(source, &kMarker, 1) == 1);

  int duplicate = fcntl(source, F_DUPFD, kLastWasmfsDescriptor);
  assert(duplicate == kLastWasmfsDescriptor);

  // The last descriptor is occupied, so no descriptor at or above the minimum
  // remains within the fixed WasmFS descriptor table.
  errno = 0;
  assert(fcntl(source, F_DUPFD, kLastWasmfsDescriptor) == -1);
  assert(errno == EMFILE);

  errno = 0;
  assert(fcntl(source, F_DUPFD, kFirstInvalidWasmfsDescriptor) == -1);
  assert(errno == EINVAL);

  // Failed duplication attempts must not invalidate the source descriptor.
  char marker = '\0';
  assert(lseek(source, 0, SEEK_SET) == 0);
  assert(read(source, &marker, 1) == 1);
  assert(marker == kMarker);

  assert(close(duplicate) == 0);
  assert(close(source) == 0);
  assert(unlink(kPath) == 0);

  printf("ok\n");
}
