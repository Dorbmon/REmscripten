// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Browser-coordinated proof for the non-mountable V4 manifest primitive. It
// stores an opaque manifest blob larger than V3's fixed 16 KiB payload and
// never changes OPFS from host JavaScript. Controlled iframe disposal after
// the first or mirrored phase witness lets a fresh document prove the allowed
// old/new outcome, then publish and re-open a later manifest. The embedded
// root and high-water fields are opaque test payload bytes: this is not a
// filesystem, directory, database, browser-crash, power-loss, or Chromium
// profile-recovery claim.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_MUTATOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_CORRUPTOR) && \
  !defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTOR)
#error "select one V4 profile-log manifest test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_MUTATOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_CORRUPTOR) + \
      defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTOR) != 1
#error "select exactly one V4 profile-log manifest test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROFILE_NAME
#error "select a V4 profile-log manifest test profile name"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST
#define WASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST 0
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_STRINGIFY_IMPL(value)

enum TestRole {
  kOwner,
  kMutator,
  kVerifier,
  kInterrupt,
  kCorruptor,
  kLiveCorruptor,
};

enum TestResult {
  kReady,
  kCorruptionRejected,
};

enum ManifestKind {
  kInitialManifest,
  kNewManifest,
  kPostRecoveryManifest,
};

enum {
  // Each image deliberately exceeds V3's single-payload capacity. Keeping
  // the maximum static avoids stack growth inside PROXY_TO_PTHREAD workers.
  kInitialManifestSize = 128 * 1024 + 257,
  kNewManifestSize = 192 * 1024 + 513,
  kPostRecoveryManifestSize = 160 * 1024 + 769,
  kManifestBufferSize = kNewManifestSize,
  kManifestHeaderSize = 32,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_TEST_PROFILE_NAME);

static const uint64_t kInitialRoot = UINT64_C(0x1122334455667788);
static const uint64_t kNewRoot = UINT64_C(0x99aabbccddeeff00);
static const uint64_t kPostRecoveryRoot = UINT64_C(0x7f6e5d4c3b2a1908);
static const uint64_t kInitialHighWater = UINT64_C(0x0000000000020001);
static const uint64_t kNewHighWater = UINT64_C(0x0000000000040003);
static const uint64_t kPostRecoveryHighWater = UINT64_C(0x0000000000060007);
static uint8_t manifestBuffer[kManifestBufferSize];
static uint8_t expectedBuffer[kManifestBufferSize];

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int result, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $2,
        event: 'result',
        result: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-manifest',
      },
      window.location.origin);
  }, role, result, error);
}

static void Report(int role, int result, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VIII, ReportOnBrowserThread, role, result, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int phase) {
  EM_ASM({
    window.parent.postMessage(
      {
        event: 'interrupt',
        phase: $0,
        type: 'wasmfs-opfs-profile-log-v4-manifest',
      },
      window.location.origin);
  }, phase);
}
#endif

// The V4 library references this private symbol only in its matching
// interruption variation. Ordinary role binaries provide a no-op so that the
// test does not add a production hook or host-side mutation route.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT_PHASE
  if (checkpoint != WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT_PHASE) {
    return;
  }
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VI, ReportInterruptionOnBrowserThread, checkpoint);
  while (1) {
    emscripten_thread_sleep(1000);
  }
#else
  (void)checkpoint;
#endif
}

static void WriteU64LE(uint8_t* bytes, uint64_t value) {
  for (size_t index = 0; index != sizeof(value); ++index) {
    bytes[index] = value >> (8 * index);
  }
}

static uint64_t ReadU64LE(const uint8_t* bytes) {
  uint64_t value = 0;
  for (size_t index = 0; index != sizeof(value); ++index) {
    value |= (uint64_t)bytes[index] << (8 * index);
  }
  return value;
}

static size_t ManifestSize(enum ManifestKind kind) {
  switch (kind) {
    case kInitialManifest:
      return kInitialManifestSize;
    case kNewManifest:
      return kNewManifestSize;
    case kPostRecoveryManifest:
      return kPostRecoveryManifestSize;
  }
  return 0;
}

static uint64_t ManifestRoot(enum ManifestKind kind) {
  switch (kind) {
    case kInitialManifest:
      return kInitialRoot;
    case kNewManifest:
      return kNewRoot;
    case kPostRecoveryManifest:
      return kPostRecoveryRoot;
  }
  return 0;
}

static uint64_t ManifestHighWater(enum ManifestKind kind) {
  switch (kind) {
    case kInitialManifest:
      return kInitialHighWater;
    case kNewManifest:
      return kNewHighWater;
    case kPostRecoveryManifest:
      return kPostRecoveryHighWater;
  }
  return 0;
}

static uint8_t ManifestSalt(enum ManifestKind kind) {
  switch (kind) {
    case kInitialManifest:
      return 0x31;
    case kNewManifest:
      return 0x72;
    case kPostRecoveryManifest:
      return 0xa5;
  }
  return 0;
}

static void MakeManifest(enum ManifestKind kind, uint8_t* bytes) {
  const size_t size = ManifestSize(kind);
  const uint8_t salt = ManifestSalt(kind);
  for (size_t index = 0; index != size; ++index) {
    bytes[index] = (uint8_t)(salt + index * 29 + (index >> 7) * 17);
  }
  // The opaque test manifest carries enough structured material to detect a
  // torn old/new image rather than merely comparing a short prefix.
  memcpy(bytes, "WASMFSV4MANIFEST", 16);
  WriteU64LE(bytes + 16, ManifestRoot(kind));
  WriteU64LE(bytes + 24, ManifestHighWater(kind));
}

static int CheckManifest(enum ManifestKind kind,
                         const uint8_t* bytes,
                         size_t size) {
  const size_t expectedSize = ManifestSize(kind);
  if (size != expectedSize || size < kManifestHeaderSize) {
    return EIO;
  }
  MakeManifest(kind, expectedBuffer);
  if (memcmp(bytes, expectedBuffer, expectedSize) != 0 ||
      memcmp(bytes, "WASMFSV4MANIFEST", 16) != 0 ||
      ReadU64LE(bytes + 16) != ManifestRoot(kind) ||
      ReadU64LE(bytes + 24) != ManifestHighWater(kind)) {
    return EIO;
  }
  return 0;
}

static enum ManifestKind ExpectedManifest(void) {
#if WASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST == 0
  return kInitialManifest;
#elif WASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST == 1
  return kNewManifest;
#elif WASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST == 2
  return kPostRecoveryManifest;
#else
#error "invalid V4 profile-log expected manifest selector"
#endif
}

static int OpenManifest(backend_t* backend) {
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_manifest_backend(kProfileName);
  return *backend ? 0 : ErrorOrEIO();
}

static int ReadManifest(backend_t backend, enum ManifestKind expectedKind) {
  const size_t expectedSize = ManifestSize(expectedKind);
  size_t required = 0;
  int result = wasmfs_opfs_profile_log_v4_read_manifest(
    backend, NULL, 0, &required);
  if (result != 0 || required != expectedSize) {
    return result < 0 ? -result : EIO;
  }

  // Querying the size must not hide a short-buffer truncation. This matters
  // for the future manifest parser, which must allocate only after it knows
  // the selected immutable image's full logical length.
  if (expectedSize > 1) {
    size_t shortRequired = 0;
    result = wasmfs_opfs_profile_log_v4_read_manifest(
      backend, manifestBuffer, expectedSize - 1, &shortRequired);
    if (result != -ENOBUFS || shortRequired != expectedSize) {
      return result < 0 ? -result : EIO;
    }
  }

  size_t size = 0;
  result = wasmfs_opfs_profile_log_v4_read_manifest(
    backend, manifestBuffer, sizeof(manifestBuffer), &size);
  return result == 0 ? CheckManifest(expectedKind, manifestBuffer, size)
                     : result < 0 ? -result : EIO;
}

static int CommitManifest(backend_t backend, enum ManifestKind kind) {
  const size_t size = ManifestSize(kind);
  MakeManifest(kind, manifestBuffer);
  const int result = wasmfs_opfs_profile_log_v4_commit_manifest(
    backend, manifestBuffer, size);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainManifest(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {0};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static int CheckManifestBoundary(backend_t manifest) {
  // V4 is a logical-manifest control plane only. It must never be mountable
  // until a complete filesystem backend separately opts into the generic
  // namespace/metadata transaction contracts.
  if (wasmfs_create_directory("/v4-manifest-not-mountable", 0700, manifest) !=
      -EIO) {
    return EIO;
  }

  // The exact V4 backend type is required. A foreign backend must reject the
  // API explicitly rather than accept opaque profile data accidentally.
  backend_t ordinary = wasmfs_create_memory_backend();
  uint8_t byte = 0;
  size_t size = 0;
  if (!ordinary ||
      wasmfs_opfs_profile_log_v4_read_manifest(
        ordinary, &byte, sizeof(byte), &size) != -ENOTSUP ||
      wasmfs_opfs_profile_log_v4_commit_manifest(ordinary, &byte,
                                                  sizeof(byte)) != -ENOTSUP) {
    return EIO;
  }
  return 0;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = 0;
  int result = kReady;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER)
  error = OpenManifest(&backend);
  if (error == 0) {
    error = CheckManifestBoundary(backend);
  }
  if (error == 0) {
    error = CommitManifest(backend, kInitialManifest);
  }
  if (error == 0) {
    error = ReadManifest(backend, kInitialManifest);
  }
  if (backend) {
    const int drainError = DrainManifest(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kOwner, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_MUTATOR)
  error = OpenManifest(&backend);
  if (error == 0) {
    error = ReadManifest(backend, kInitialManifest);
  }
  if (error == 0) {
    error = CommitManifest(backend, kNewManifest);
  }
  if (error == 0) {
    error = ReadManifest(backend, kNewManifest);
  }
  if (backend) {
    const int drainError = DrainManifest(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kMutator, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER)
  error = OpenManifest(&backend);
  if (error == 0) {
    error = ReadManifest(backend, ExpectedManifest());
  }
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_TEST_POST_RECOVERY_MUTATION
  if (error == 0) {
    error = CommitManifest(backend, kPostRecoveryManifest);
  }
  if (error == 0) {
    error = ReadManifest(backend, kPostRecoveryManifest);
  }
#endif
  if (backend) {
    const int drainError = DrainManifest(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kVerifier, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPTOR)
  error = OpenManifest(&backend);
  if (error == 0) {
    error = ReadManifest(backend, kInitialManifest);
  }
  if (error == 0) {
    // In the matching test build this call reports the chosen witness and
    // does not return before the parent disposes the iframe.
    error = CommitManifest(backend, kNewManifest);
  }
  if (backend) {
    const int drainError = DrainManifest(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  Report(kInterrupt, result, error);
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTOR)
  // The matching system-library variation corrupts only its local validation
  // copy after this factory has accepted real OPFS state. A live read must
  // reject it and permanently poison the instance before a later commit.
  error = OpenManifest(&backend);
  if (error == 0) {
    size_t size = 0;
    if (wasmfs_opfs_profile_log_v4_read_manifest(
          backend, NULL, 0, &size) != -EIO) {
      error = EIO;
    }
  }
  if (error == 0) {
    const uint8_t byte = 0;
    if (wasmfs_opfs_profile_log_v4_commit_manifest(
          backend, &byte, sizeof(byte)) != -EIO) {
      error = EIO;
    }
  }
  // The expected integrity failure is terminal, so a scoped orderly drain
  // reports that same fatal error rather than falsely acknowledging a clean
  // handoff. The parent disposes this iframe and retries only after the
  // browser has released its worker-owned lease.
  result = kCorruptionRejected;
  Report(kLiveCorruptor, result, error);
#else
  // Selected-record faults are injected only into the matching system-library
  // variation after native reads. This role must see a failed factory before
  // the opaque manifest can be exposed to an application caller.
  errno = 0;
  backend = wasmfs_create_opfs_profile_log_v4_manifest_backend(kProfileName);
  if (backend || errno != EIO) {
    error = EIO;
  }
  if (backend) {
    const int drainError = DrainManifest(backend);
    if (error == 0) {
      error = drainError;
    }
  }
  result = kCorruptionRejected;
  Report(kCorruptor, result, error);
#endif

  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
