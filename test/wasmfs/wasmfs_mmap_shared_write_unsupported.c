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
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
  static const char contents[] = "mmap data";
  int rw = open("mmap-data", O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(rw >= 0);
  assert(write(rw, contents, sizeof(contents)) == sizeof(contents));

  // Valid file-backed shared writable mappings must fail rather than accept
  // incomplete mapping lifetime, coherence, and writeback semantics.
  errno = 0;
  void* mapping = mmap(
    NULL, sizeof(contents), PROT_READ | PROT_WRITE, MAP_SHARED, rw, 0);
  assert(mapping == MAP_FAILED);
  assert(errno == ENOTSUP);

  // Keep read-only shared snapshot mappings available.
  mapping = mmap(NULL, sizeof(contents), PROT_READ, MAP_SHARED, rw, 0);
  assert(mapping != MAP_FAILED);
  assert(memcmp(mapping, contents, sizeof(contents)) == 0);
  assert(munmap(mapping, sizeof(contents)) == 0);

  int ro = open("mmap-data", O_RDONLY);
  assert(ro >= 0);

  // Preserve normal mmap error precedence before the unsupported-operation
  // check and permit copy-on-write mappings of read-only files.
  errno = 0;
  mapping = mmap(
    NULL, sizeof(contents), PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
  assert(mapping == MAP_FAILED);
  assert(errno == EACCES);

  errno = 0;
  mapping = mmap(
    NULL, sizeof(contents), PROT_READ | PROT_WRITE, MAP_SHARED, -1, 0);
  assert(mapping == MAP_FAILED);
  assert(errno == EBADF);

  mapping = mmap(
    NULL, sizeof(contents), PROT_READ | PROT_WRITE, MAP_PRIVATE, ro, 0);
  assert(mapping != MAP_FAILED);
  ((char*)mapping)[0] = 'M';
  assert(munmap(mapping, sizeof(contents)) == 0);
  assert(close(ro) == 0);

  char readback[sizeof(contents)];
  assert(pread(rw, readback, sizeof(readback), 0) == sizeof(readback));
  assert(memcmp(readback, contents, sizeof(contents)) == 0);

  // File-backed WasmFS rejection must not affect anonymous shared mappings.
  mapping = mmap(NULL,
                 4096,
                 PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS,
                 -1,
                 0);
  assert(mapping != MAP_FAILED);
  ((char*)mapping)[0] = 'a';
  assert(((char*)mapping)[0] == 'a');
  assert(munmap(mapping, 4096) == 0);

  assert(close(rw) == 0);
  assert(unlink("mmap-data") == 0);
  printf("ok\n");
}
