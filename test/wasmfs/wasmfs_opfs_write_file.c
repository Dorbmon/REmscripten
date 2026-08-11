// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

enum TestRole {
  kHolder,
  kContender,
};

static const char kPath[] = "/opfs/wasmfs-opfs-write-file-target";
static const char kRawFirst[] = "raw-first";
static const char kRawSecond[] = "raw-second";
static const char kPublicContents[] = "public";
static const char kPublicAppendedContents[] = "public!";
static const char kContenderContents[] = "contender";

#ifdef WASMFS_OPFS_WRITE_FILE_HOLDER
static _Atomic int shutdown_requested;
static int holder_error;
#endif

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

// These imports run from the C application sequence. Under PROXY_TO_PTHREAD
// that is the module's runtime pthread, not the host-page controller.
EM_JS(int, WriteFile, (const char* path, const char* contents, int expected), {
  if (!ENVIRONMENT_IS_PTHREAD) {
    return -1;
  }
  try {
    const result = FS.writeFile(UTF8ToString(path), UTF8ToString(contents));
    return expected ? -1 : result === undefined ? 0 : -1;
  } catch (e) {
    if (!expected || e.name != 'ErrnoError') {
      return -1;
    }
    return e.errno == expected ? 0 : e.errno || -1;
  }
});

EM_JS(int, AppendFile, (const char* path, const char* contents), {
  if (!ENVIRONMENT_IS_PTHREAD) {
    return -1;
  }
  try {
    const result = FS.writeFile(UTF8ToString(path), UTF8ToString(contents),
                                {flags: 'a'});
    return result === undefined ? 0 : -1;
  } catch {
    return -1;
  }
});

EM_JS(int, VerifyFile, (const char* path, const char* expected), {
  if (!ENVIRONMENT_IS_PTHREAD) {
    return -1;
  }
  try {
    return FS.readFile(UTF8ToString(path), {encoding: 'utf8'}) ==
                   UTF8ToString(expected) ?
             0 :
             -1;
  } catch {
    return -1;
  }
});

// This is the residual low-level WasmFS export. It receives the C strings'
// in-memory addresses directly, so this verifies its descriptor route without
// calling the public FS.writeFile wrapper.
EM_JS(int, RawWriteFile,
      (const char* path, const char* contents, int length), {
  if (!ENVIRONMENT_IS_PTHREAD) {
    return -1;
  }
  return __wasmfs_write_file(path, contents, length);
});

static int NormalizeJSResult(int result) {
  return result == 0 ? 0 : result > 0 ? result : EIO;
}

static void ReportResultOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-write-file',
      },
      window.location.origin);
  }, role, error);
}

static void ReportResult(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportResultOnBrowserThread, role, error);
}

static int MountOPFS(void) {
  backend_t backend = wasmfs_create_opfs_backend();
  if (!backend) {
    return ErrorOrEIO();
  }

  int err = wasmfs_create_directory("/opfs", 0777, backend);
  if (err != 0) {
    return err < 0 ? -err : EIO;
  }
  return 0;
}

#ifdef WASMFS_OPFS_WRITE_FILE_HOLDER

EMSCRIPTEN_KEEPALIVE
void wasmfs_opfs_write_file_holder_shutdown(void) {
  atomic_store(&shutdown_requested, 1);
}

static int ExerciseNormalHolder(void) {
  int raw_length = (int)strlen(kRawFirst);
  if (RawWriteFile(kPath, kRawFirst, raw_length) != raw_length) {
    return EIO;
  }
  raw_length = (int)strlen(kRawSecond);
  if (RawWriteFile(kPath, kRawSecond, raw_length) != raw_length ||
      VerifyFile(kPath, kRawSecond) != 0) {
    return EIO;
  }

  if (NormalizeJSResult(WriteFile(kPath, kPublicContents, 0)) != 0 ||
      AppendFile(kPath, "!") != 0 ||
      VerifyFile(kPath, kPublicAppendedContents) != 0) {
    return EIO;
  }
  return 0;
}

static int ExerciseQuotaHolder(void) {
  int raw_length = (int)strlen(kRawFirst);
  if (RawWriteFile(kPath, kRawFirst, raw_length) != -ENOSPC) {
    return EIO;
  }
  if (VerifyFile(kPath, "") != 0 ||
      NormalizeJSResult(WriteFile(kPath, kPublicContents, ENOSPC)) != 0 ||
      VerifyFile(kPath, "") != 0) {
    return EIO;
  }
  return 0;
}

static int ExerciseCloseFailureHolder(void) {
  // Both hooks are active for this build. The write reports ENOSPC first, but
  // the mandatory close then fails and must become the public EIO result.
  return NormalizeJSResult(WriteFile(kPath, kPublicContents, EIO));
}

static void ShutdownWhenRequested(void) {
  if (atomic_exchange(&shutdown_requested, 0)) {
    exit(holder_error == 0 ? 0 : 1);
  }
}

#else

static void KeepRuntimeAlive(void) {}

#endif

int main(void) {
  int error = MountOPFS();

#ifdef WASMFS_OPFS_WRITE_FILE_HOLDER
  if (error == 0) {
    // Each holder begins with a fresh pathname. The controller keeps this
    // module alive while a second module attempts the same underlying OPFS
    // entry through a fresh backend and wrapper.
    (void)unlink(kPath);
#if defined(WASMFS_OPFS_WRITE_FILE_CLOSE_FAILURE)
    error = ExerciseCloseFailureHolder();
#elif defined(WASMFS_OPFS_WRITE_FILE_QUOTA)
    error = ExerciseQuotaHolder();
#else
    error = ExerciseNormalHolder();
#endif
  }
  holder_error = error;
  ReportResult(kHolder, error);
  emscripten_set_main_loop(ShutdownWhenRequested, 0, false);
#else
#if defined(WASMFS_OPFS_WRITE_FILE_CLOSE_FAILURE)
  if (error == 0) {
    // The holder's rejected close deliberately leaves the browser writer live.
    // A fresh module must receive the OPFS writer-exclusion error instead of
    // silently obtaining a second SyncAccessHandle.
    error = NormalizeJSResult(WriteFile(kPath, kContenderContents, EACCES));
  }
#else
  if (error == 0) {
    error = NormalizeJSResult(WriteFile(kPath, kContenderContents, 0));
    if (error == 0 && VerifyFile(kPath, kContenderContents) != 0) {
      error = EIO;
    }
  }
#endif
  ReportResult(kContender, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
#endif
}
