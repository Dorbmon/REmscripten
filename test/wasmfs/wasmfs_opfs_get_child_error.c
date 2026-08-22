/*
 * Copyright 2026 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/wasmfs.h>

static const char kMountPath[] = "/opfs";
static const char kInjectedErrorPath[] =
  "/opfs/__wasmfs_opfs_test_get_child_eio__";
static const char kProxyFailurePath[] =
  "/opfs/__wasmfs_opfs_test_get_child_proxy_failure__";
static const char kMalformedFilePath[] =
  "/opfs/__wasmfs_opfs_test_get_child_malformed_file__";
static const char kMalformedDirectoryPath[] =
  "/opfs/__wasmfs_opfs_test_get_child_malformed_directory__";
static const char kMissingPath[] = "/opfs/missing";
static const char kCreatedPath[] = "/opfs/created";

static int entryIsAbsent(const char* name) {
  DIR* directory = opendir(kMountPath);
  assert(directory);
  int absent = 1;
  errno = 0;
  for (struct dirent* entry; (entry = readdir(directory));) {
    if (strcmp(entry->d_name, name) == 0) {
      absent = 0;
      break;
    }
  }
  assert(errno == 0);
  assert(closedir(directory) == 0);
  return absent;
}

static void assertLookupAndCreationFailWithEIO(const char* path,
                                               const char* name) {
  errno = 0;
  assert(access(path, F_OK) == -1);
  assert(errno == EIO);

  errno = 0;
  assert(open(path, O_CREAT | O_EXCL | O_RDWR, 0600) == -1);
  assert(errno == EIO);
  errno = 0;
  assert(access(path, F_OK) == -1);
  assert(errno == EIO);
  assert(entryIsAbsent(name));
}

int main(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  assert(backend);
  assert(wasmfs_create_directory(kMountPath, 0700, backend) == 0);

  // Lookups that fail in JS, whose proxy never begins, or whose bridge output
  // is malformed must all reject creation without changing either the dcache
  // or OPFS namespace.
  assertLookupAndCreationFailWithEIO(
    kInjectedErrorPath, "__wasmfs_opfs_test_get_child_eio__");
  assertLookupAndCreationFailWithEIO(
    kProxyFailurePath, "__wasmfs_opfs_test_get_child_proxy_failure__");
  assertLookupAndCreationFailWithEIO(
    kMalformedFilePath, "__wasmfs_opfs_test_get_child_malformed_file__");
  assertLookupAndCreationFailWithEIO(
    kMalformedDirectoryPath,
    "__wasmfs_opfs_test_get_child_malformed_directory__");

  // A real missing child remains distinguishable from the injected I/O error.
  errno = 0;
  assert(access(kMissingPath, F_OK) == -1);
  assert(errno == ENOENT);

  // The failed lookup must not poison later normal creation or lookup.
  int fd = open(kCreatedPath, O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);
  assert(access(kCreatedPath, F_OK) == 0);
  assert(unlink(kCreatedPath) == 0);
  return 0;
}
