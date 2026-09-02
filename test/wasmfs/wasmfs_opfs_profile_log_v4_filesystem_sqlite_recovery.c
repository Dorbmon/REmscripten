// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  See the LICENSE file for
// details.

// Fresh-document SQLite rollback-journal recovery proof for the V4 mountable
// profile filesystem.  The interruptor arms an existing V4 durable-publication
// cut immediately before SQLite's COMMIT of B.  A new document must recover a
// complete A or B pair, never a mixed pair, then persist C for another new
// document.  This is controlled iframe-disposal evidence.  It does not prove
// physical power-loss behavior, WAL/shared-mmap support, LevelDB, Chromium
// profile activation, or M7 completion.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <sqlite3.h>

#if !defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_SEED) && \
    !defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPTOR) && \
    !defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_VERIFIER) && \
    !defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD) && \
    !defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_CONTROL)
#error "select one V4 SQLite recovery test role"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_SEED) + \
        defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPTOR) + \
        defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_VERIFIER) + \
        defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD) + \
        defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_CONTROL) != 1
#error "select exactly one V4 SQLite recovery test role"
#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_PROFILE_NAME
#error "select a V4 SQLite recovery test profile name"
#endif

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPTOR) && \
    !defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE)
#error "select a V4 SQLite recovery interruption phase"
#endif

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE
#if WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE != 10 && \
    WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE != 1 && \
    WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE != 2
#error "select a V4 descriptor or phase publication interruption"
#endif

#endif

#ifndef WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD_STATE
#define WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD_STATE 3
#endif

#if WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD_STATE != 2 && \
    WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD_STATE != 3
#error "select B or C as the V4 SQLite recovery reload state"
#endif

#define WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_STRINGIFY_IMPL(value) #value
#define WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_STRINGIFY(value) \
  WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_STRINGIFY_IMPL(value)

enum TestRole {
  kSeed,
  kInterruptor,
  kVerifier,
  kReload,
  kControl,
};

enum DatabaseState {
  kStateA = 1,
  kStateB = 2,
  kStateC = 3,
};

enum {
  kRowCount = 2,
  kPayloadSize = 2048,
};

static const char kProfileName[] =
  WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_STRINGIFY(
    WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_PROFILE_NAME);
static const char kMountPath[] = "/v4fs-sqlite-recovery";
static const char kDatabasePath[] = "/v4fs-sqlite-recovery/profile.sqlite";
static const char kJournalPath[] =
  "/v4fs-sqlite-recovery/profile.sqlite-journal";

// The outer V4 hook is available only in source-selected interruption builds.
// It is armed only after the B updates have been prepared and immediately
// before sqlite3_exec(COMMIT) begins on this pthread.  The test observes a V4
// publication boundary reached during COMMIT, not a SQLite VFS xSync hook.
static _Atomic int interruption_armed;
static _Atomic int interruption_fired;
static pthread_t interruption_thread;

static int ErrorOrEIO(void) { return errno ? errno : EIO; }

static void ReportOnBrowserThread(int role, int error) {
  EM_ASM({
    window.parent.postMessage(
      {
        error: $1,
        role: $0,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-sqlite-recovery',
      },
      window.location.origin);
  }, role, error);
}

static void Report(int role, int error) {
  emscripten_async_run_in_main_runtime_thread(
    EM_FUNC_SIG_VII, ReportOnBrowserThread, role, error);
}

#ifdef WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE
static void ReportInterruptionOnBrowserThread(int checkpoint) {
  EM_ASM({
    window.parent.postMessage(
      {
        checkpoint: $0,
        error: 0,
        event: 'interrupt',
        role: 1,
        type: 'wasmfs-opfs-profile-log-v4-filesystem-sqlite-recovery',
      },
      window.location.origin);
  }, checkpoint);
}
#endif

// This private link symbol is supplied only by this test executable.  It is
// inert until COMMIT B has started, and it accepts exactly one ordinary V4
// descriptor/phase checkpoint from that committing pthread.
void wasmfs_opfs_profile_log_v4_test_maybe_interrupt(int checkpoint) {
#ifdef WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE
  if (!atomic_load(&interruption_armed) ||
      checkpoint !=
        WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE ||
      !pthread_equal(pthread_self(), interruption_thread)) {
    return;
  }
  int expected = 0;
  if (!atomic_compare_exchange_strong(&interruption_fired, &expected, 1)) {
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

static int CloseChecked(int fd) {
  return close(fd) == 0 ? 0 : ErrorOrEIO();
}

static int MountFilesystem(backend_t* backend) {
  if (!backend) {
    return EINVAL;
  }
  errno = 0;
  *backend = wasmfs_create_opfs_profile_log_v4_filesystem_backend(kProfileName);
  if (!*backend) {
    return ErrorOrEIO();
  }
  const int result = wasmfs_create_directory(kMountPath, 0700, *backend);
  return result == 0 ? 0 : result < 0 ? -result : EIO;
}

static int DrainFilesystem(backend_t backend) {
  wasmfs_opfs_profile_drain_result details = {};
  const int result = wasmfs_drain_opfs_profile_backend(backend, &details);
  return result == 0 && details.error == 0 && details.backend_sealed &&
         details.lease_released && details.backend_retired &&
         details.data_file_states == 0 && details.backend_retire_failures == 0
           ? 0
           : EIO;
}

static int FlushDirectory(void) {
  const int fd = open(kMountPath, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return ErrorOrEIO();
  }
  const int error = fsync(fd) == 0 ? 0 : ErrorOrEIO();
  const int close_error = CloseChecked(fd);
  return error ? error : close_error;
}

static int SQLiteError(int result) {
  return result == SQLITE_OK || result == SQLITE_ROW || result == SQLITE_DONE
           ? 0
           : EIO;
}

static int Exec(sqlite3* database, const char* sql) {
  if (!database || !sql) {
    return EINVAL;
  }
  char* message = NULL;
  const int result = sqlite3_exec(database, sql, NULL, NULL, &message);
  sqlite3_free(message);
  return SQLiteError(result);
}

static int OpenDatabase(sqlite3** database, int create) {
  if (!database) {
    return EINVAL;
  }
  *database = NULL;
  int flags = SQLITE_OPEN_READWRITE;
  if (create) {
    flags |= SQLITE_OPEN_CREATE;
  }
  const int result = sqlite3_open_v2(kDatabasePath, database, flags, NULL);
  if (result != SQLITE_OK) {
    if (*database) {
      sqlite3_close(*database);
      *database = NULL;
    }
    return EIO;
  }
  return 0;
}

static int CloseDatabase(sqlite3** database) {
  if (!database || !*database) {
    return EINVAL;
  }
  const int result = sqlite3_close(*database);
  if (result != SQLITE_OK) {
    return EIO;
  }
  *database = NULL;
  return 0;
}

static int QueryText(sqlite3* database,
                     const char* sql,
                     const char* expected) {
  if (!database || !sql || !expected) {
    return EINVAL;
  }
  sqlite3_stmt* statement = NULL;
  int error = SQLiteError(sqlite3_prepare_v2(database, sql, -1, &statement,
                                              NULL));
  if (!error && sqlite3_step(statement) != SQLITE_ROW) {
    error = EIO;
  }
  const unsigned char* actual = !error ? sqlite3_column_text(statement, 0)
                                        : NULL;
  if (!error && (!actual || strcmp((const char*)actual, expected) != 0)) {
    error = EIO;
  }
  if (!error && sqlite3_step(statement) != SQLITE_DONE) {
    error = EIO;
  }
  if (statement && sqlite3_finalize(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  return error;
}

static int QueryInteger(sqlite3* database, const char* sql, int expected) {
  if (!database || !sql) {
    return EINVAL;
  }
  sqlite3_stmt* statement = NULL;
  int error = SQLiteError(sqlite3_prepare_v2(database, sql, -1, &statement,
                                              NULL));
  if (!error && sqlite3_step(statement) != SQLITE_ROW) {
    error = EIO;
  }
  if (!error && sqlite3_column_int(statement, 0) != expected) {
    error = EIO;
  }
  if (!error && sqlite3_step(statement) != SQLITE_DONE) {
    error = EIO;
  }
  if (statement && sqlite3_finalize(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  return error;
}

static int ConfigureSeedDatabase(sqlite3* database) {
  int error = Exec(database, "PRAGMA page_size=512");
  if (!error) {
    error = Exec(database, "PRAGMA journal_mode=DELETE");
  }
  if (!error) {
    error = QueryText(database, "PRAGMA journal_mode", "delete");
  }
  if (!error) {
    error = Exec(database, "PRAGMA synchronous=FULL");
  }
  if (!error) {
    error = QueryInteger(database, "PRAGMA synchronous", 2);
  }
  if (!error) {
    error = Exec(database, "PRAGMA temp_store=MEMORY");
  }
  return error;
}

static int ConfigureOpenDatabase(sqlite3* database) {
  int error = QueryText(database, "PRAGMA journal_mode", "delete");
  if (!error) {
    error = Exec(database, "PRAGMA synchronous=FULL");
  }
  if (!error) {
    error = QueryInteger(database, "PRAGMA synchronous", 2);
  }
  if (!error) {
    error = Exec(database, "PRAGMA temp_store=MEMORY");
  }
  return error;
}

static void MakePayload(int state, int slot, uint8_t* bytes) {
  for (size_t index = 0; index != kPayloadSize; ++index) {
    bytes[index] = (uint8_t)(state * 37 + slot * 19 + index * 13);
  }
}

static int StepStateStatement(sqlite3_stmt* statement, int state, int slot) {
  uint8_t payload[kPayloadSize] = {};
  MakePayload(state, slot, payload);
  int error = SQLiteError(sqlite3_bind_blob(
    statement, 1, payload, sizeof(payload), SQLITE_TRANSIENT));
  if (!error) {
    error = SQLiteError(sqlite3_bind_int(statement, 2, state));
  }
  if (!error) {
    error = SQLiteError(sqlite3_bind_int(statement, 3, slot));
  }
  if (!error && sqlite3_step(statement) != SQLITE_DONE) {
    error = EIO;
  }
  if (sqlite3_reset(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  if (sqlite3_clear_bindings(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  return error;
}

static int WriteState(sqlite3* database, int state, int insert) {
  const char* statement_sql = insert
    ? "INSERT INTO profile_state(slot, payload, revision) VALUES(?3, ?1, ?2)"
    : "UPDATE profile_state SET payload=?1, revision=?2 WHERE slot=?3";
  int error = Exec(database, "BEGIN IMMEDIATE");
  sqlite3_stmt* statement = NULL;
  if (!error) {
    error = SQLiteError(sqlite3_prepare_v2(database, statement_sql, -1,
                                            &statement, NULL));
  }
  for (int slot = 1; !error && slot <= kRowCount; ++slot) {
    error = StepStateStatement(statement, state, slot);
    if (!error && !insert && sqlite3_changes(database) != 1) {
      error = EIO;
    }
  }
  if (statement && sqlite3_finalize(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  if (error) {
    (void)Exec(database, "ROLLBACK");
    return error;
  }
  return Exec(database, "COMMIT");
}

static int VerifyIntegrity(sqlite3* database) {
  return QueryText(database, "PRAGMA integrity_check", "ok");
}

static int ReadState(sqlite3* database, int* state) {
  if (!database || !state) {
    return EINVAL;
  }
  sqlite3_stmt* statement = NULL;
  int error = SQLiteError(sqlite3_prepare_v2(
    database, "SELECT slot, payload, revision FROM profile_state ORDER BY slot",
    -1, &statement, NULL));
  int found_state = 0;
  for (int expected_slot = 1; !error && expected_slot <= kRowCount;
       ++expected_slot) {
    if (sqlite3_step(statement) != SQLITE_ROW) {
      error = EIO;
      break;
    }
    const int slot = sqlite3_column_int(statement, 0);
    const void* payload = sqlite3_column_blob(statement, 1);
    const int payload_size = sqlite3_column_bytes(statement, 1);
    const int revision = sqlite3_column_int(statement, 2);
    if (slot != expected_slot || !payload || payload_size != kPayloadSize ||
        revision < kStateA || revision > kStateC ||
        (found_state && revision != found_state)) {
      error = EIO;
      break;
    }
    uint8_t expected[kPayloadSize] = {};
    MakePayload(revision, slot, expected);
    if (memcmp(payload, expected, sizeof(expected)) != 0) {
      error = EIO;
      break;
    }
    found_state = revision;
  }
  if (!error && sqlite3_step(statement) != SQLITE_DONE) {
    error = EIO;
  }
  if (statement && sqlite3_finalize(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  if (!error) {
    *state = found_state;
  }
  return error;
}

static int VerifyExactState(sqlite3* database, int expected_state) {
  int state = 0;
  int error = VerifyIntegrity(database);
  if (!error) {
    error = ReadState(database, &state);
  }
  return !error && state == expected_state ? 0 : error ? error : EIO;
}

static int VerifyActiveRollbackJournal(void) {
  struct stat details = {};
  if (stat(kJournalPath, &details) != 0) {
    return ErrorOrEIO();
  }
  return S_ISREG(details.st_mode) && details.st_size > 0 ? 0 : EIO;
}

static int VerifyCompleteAOrB(sqlite3* database) {
  int state = 0;
  int error = VerifyIntegrity(database);
  if (!error) {
    error = ReadState(database, &state);
  }
  return !error && (state == kStateA || state == kStateB)
           ? 0
           : error ? error : EIO;
}

static int SeedDatabase(void) {
  sqlite3* database = NULL;
  int error = OpenDatabase(&database, 1);
  if (!error) {
    error = ConfigureSeedDatabase(database);
  }
  if (!error) {
    error = Exec(database,
      "CREATE TABLE profile_state("
      "slot INTEGER PRIMARY KEY, payload BLOB NOT NULL, revision INTEGER NOT NULL)");
  }
  if (!error) {
    error = WriteState(database, kStateA, 1);
  }
  if (!error) {
    error = VerifyExactState(database, kStateA);
  }
  if (database) {
    const int close_error = CloseDatabase(&database);
    if (!error) {
      error = close_error;
    }
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int OpenAndVerifyA(sqlite3** database) {
  int error = OpenDatabase(database, 0);
  if (!error) {
    error = ConfigureOpenDatabase(*database);
  }
  if (!error) {
    error = VerifyExactState(*database, kStateA);
  }
  return error;
}

static int InterruptCommitB(sqlite3* database) {
  int error = Exec(database, "BEGIN IMMEDIATE");
  sqlite3_stmt* statement = NULL;
  if (!error) {
    error = SQLiteError(sqlite3_prepare_v2(
      database, "UPDATE profile_state SET payload=?1, revision=?2 WHERE slot=?3",
      -1, &statement, NULL));
  }
  for (int slot = 1; !error && slot <= kRowCount; ++slot) {
    error = StepStateStatement(statement, kStateB, slot);
    if (!error && sqlite3_changes(database) != 1) {
      error = EIO;
    }
  }
  if (statement && sqlite3_finalize(statement) != SQLITE_OK && !error) {
    error = EIO;
  }
  if (error) {
    (void)Exec(database, "ROLLBACK");
    return error;
  }
  // The named rollback journal must exist before the V4 fault is armed.  This
  // makes DELETE journaling an observed part of the interruption witness, not
  // merely a requested PRAGMA.
  error = VerifyActiveRollbackJournal();
  if (error) {
    (void)Exec(database, "ROLLBACK");
    return error;
  }
  interruption_thread = pthread_self();
  atomic_store(&interruption_fired, 0);
  atomic_store(&interruption_armed, 1);
  // A selected V4 durable-publication hook inside this COMMIT never returns.
  // A return means it was not reached, so this source-selected witness fails.
  (void)sqlite3_exec(database, "COMMIT", NULL, NULL, NULL);
  atomic_store(&interruption_armed, 0);
  return EIO;
}

static int CommitBUninterrupted(void) {
  sqlite3* database = NULL;
  int error = OpenAndVerifyA(&database);
  if (!error) {
    error = WriteState(database, kStateB, 0);
  }
  if (!error) {
    error = VerifyExactState(database, kStateB);
  }
  if (database) {
    const int close_error = CloseDatabase(&database);
    if (!error) {
      error = close_error;
    }
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int RecoverAndCommitC(void) {
  sqlite3* database = NULL;
  int error = OpenDatabase(&database, 0);
  if (!error) {
    error = ConfigureOpenDatabase(database);
  }
  if (!error) {
    error = VerifyCompleteAOrB(database);
  }
  if (!error) {
    error = WriteState(database, kStateC, 0);
  }
  if (!error) {
    error = VerifyExactState(database, kStateC);
  }
  if (database) {
    const int close_error = CloseDatabase(&database);
    if (!error) {
      error = close_error;
    }
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static int ReloadAndVerifyExpectedState(void) {
  sqlite3* database = NULL;
  int error = OpenDatabase(&database, 0);
  if (!error) {
    error = ConfigureOpenDatabase(database);
  }
  if (!error) {
    error = VerifyExactState(
      database, WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD_STATE);
  }
  if (database) {
    const int close_error = CloseDatabase(&database);
    if (!error) {
      error = close_error;
    }
  }
  if (!error) {
    error = FlushDirectory();
  }
  return error;
}

static void KeepRuntimeAlive(void) {}

int main(void) {
  backend_t backend = NULL;
  int error = MountFilesystem(&backend);
  int role = kSeed;

#if defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_SEED)
  if (!error) {
    error = SeedDatabase();
  }
  role = kSeed;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPTOR)
  sqlite3* database = NULL;
  if (!error) {
    error = OpenAndVerifyA(&database);
  }
  if (!error) {
    error = InterruptCommitB(database);
  }
  if (database) {
    const int close_error = CloseDatabase(&database);
    if (!error) {
      error = close_error;
    }
  }
  role = kInterruptor;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_VERIFIER)
  if (!error) {
    error = RecoverAndCommitC();
  }
  role = kVerifier;
#elif defined(WASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_CONTROL)
  if (!error) {
    error = CommitBUninterrupted();
  }
  role = kControl;
#else
  if (!error) {
    error = ReloadAndVerifyExpectedState();
  }
  role = kReload;
#endif

  if (backend) {
    const int drain_error = DrainFilesystem(backend);
    if (!error) {
      error = drain_error;
    }
  }
  Report(role, error);
  emscripten_set_main_loop(KeepRuntimeAlive, 0, false);
}
