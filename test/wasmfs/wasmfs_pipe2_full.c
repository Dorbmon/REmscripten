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
static const char kSourcePath[] = "wasmfs-pipe2-full";

static void ExpectPipe2EMFILE(void) {
  int pipefd[2] = {-17, -23};
  errno = 0;
  assert(pipe2(pipefd, O_CLOEXEC) == -1);
  assert(errno == EMFILE);
  assert(pipefd[0] == -17);
  assert(pipefd[1] == -23);
}

int main(void) {
  int source = open(kSourcePath, O_CREAT | O_TRUNC | O_RDWR, 0600);
  assert(source >= 3 && source < kLastWasmfsDescriptor);

  for (int fd = 3; fd <= kLastWasmfsDescriptor; ++fd) {
    if (fd != source) {
      assert(dup3(source, fd, 0) == fd);
    }
  }

  // With no free descriptors, pipe2() must fail without modifying pipefd.
  ExpectPipe2EMFILE();

  // With one free descriptor, pipe2() must still fail, and must roll back the
  // reader it temporarily installed so the slot remains reusable.
  assert(close(kLastWasmfsDescriptor) == 0);
  ExpectPipe2EMFILE();
  errno = 0;
  assert(fcntl(kLastWasmfsDescriptor, F_GETFD) == -1);
  assert(errno == EBADF);
  assert(dup3(source, kLastWasmfsDescriptor, 0) ==
         kLastWasmfsDescriptor);

  for (int fd = 3; fd <= kLastWasmfsDescriptor; ++fd) {
    if (fd != source) {
      assert(close(fd) == 0);
    }
  }
  assert(close(source) == 0);
  assert(unlink(kSourcePath) == 0);

  int pipefd[2];
  assert(pipe2(pipefd, O_CLOEXEC) == 0);
  assert(close(pipefd[0]) == 0);
  assert(close(pipefd[1]) == 0);

  printf("ok\n");
}
