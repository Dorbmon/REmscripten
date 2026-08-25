// Copyright 2021 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// Syscall implementations.

#define _LARGEFILE64_SOURCE // For F_GETLK64 etc

#include <dirent.h>
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/html5.h>
#include <emscripten/syscalls.h>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wasi/api.h>

#include "backend.h"
#include "file.h"
#include "file_table.h"
#include "paths.h"
#include "pipe_backend.h"
#include "special_files.h"
#include "wasmfs.h"

// File permission macros for wasmfs.
// Used to improve readability compared to those in stat.h
#define WASMFS_PERM_READ 0444

#define WASMFS_PERM_WRITE 0222

#define WASMFS_PERM_EXECUTE 0111

// In Linux, the maximum length for a filename is 255 bytes.
#define WASMFS_NAME_MAX 255

namespace {

// Parent pointers are weak and each directory has its own lock. Traverse the
// CWD tree with at most one directory lock held at a time so an unmount check
// cannot invert the normal WasmFS file-locking discipline.
bool cwdIsAtOrBelow(wasmfs::WasmFS::CWDTransition& cwdTransition,
                    const std::shared_ptr<wasmfs::Directory>& directory) {
  auto current = cwdTransition.getCWD();
  while (current) {
    if (current == directory) {
      return true;
    }

    std::shared_ptr<wasmfs::Directory> parent;
    {
      auto lockedCurrent = current->locked();
      parent = lockedCurrent.getParent();
    }

    // The root is its own parent. An unlinked directory has no parent.
    if (!parent || parent == current) {
      return false;
    }
    current = std::move(parent);
  }
  return false;
}

bool canMutateExplicitMetadata(const std::shared_ptr<wasmfs::File>& file) {
  if (auto* backend = file->getBackend()) {
    return backend->supportsExplicitMetadataMutation();
  }
  return true;
}

// An opted-in backend owns a content/metadata transaction. The generic layer
// must not write or resize first and then update mtime in WasmFS memory: an
// interrupted operation would expose a persistent content image with a stale
// metadata image. Backends that do not opt in retain the historical split
// behavior while the profile backend is brought up incrementally.
bool requiresAtomicMetadataMutations(
  const std::shared_ptr<wasmfs::DataFile>& file) {
  if (auto* backend = file->getBackend()) {
    return backend->requiresAtomicMetadataMutations();
  }
  return false;
}

wasmfs::File::Metadata dataMutationMetadataPostImage(
  wasmfs::DataFile::Handle& locked) {
  auto metadata = locked.getMetadata();
  // A data or length mutation changes both mtime and ctime. The complete
  // candidate also carries mode and atime so a storage backend can replace
  // one coherent metadata record rather than reconstructing fields.
  const double now = emscripten_date_now();
  metadata.mtime = now;
  metadata.ctime = now;
  return metadata;
}

int resizeDataFile(
  const std::shared_ptr<wasmfs::DataFile>& dataFile,
  wasmfs::DataFile::Handle& locked,
  off_t size) {
  if (!requiresAtomicMetadataMutations(dataFile)) {
    return locked.setSize(size);
  }
  return locked.setSizeWithMetadata(
    size, dataMutationMetadataPostImage(locked));
}

// Record locks in POSIX are process-owned. WasmFS has one process-global
// instance, so a backend may opt in only when it has independently established
// that no other WasmFS instance can concurrently mutate the same storage.
// In particular, the ordinary OPFS backend deliberately does not opt in.
bool canUseRecordLocks(const std::shared_ptr<wasmfs::File>& file) {
  auto* backend = file->getBackend();
  return S_ISREG(file->locked().getMode()) && backend &&
         backend->supportsRecordLocks();
}

struct RecordLockRange {
  off_t start;
  std::optional<off_t> end;
};

static_assert(std::numeric_limits<off_t>::is_signed,
              "Record lock offsets must be signed");

bool addWouldOverflow(off_t lhs, off_t rhs) {
  if (rhs > 0) {
    return lhs > std::numeric_limits<off_t>::max() - rhs;
  }
  if (rhs < 0) {
    return lhs < std::numeric_limits<off_t>::min() - rhs;
  }
  return false;
}

// Convert the POSIX flock range into a half-open absolute byte range. A zero
// length locks through end-of-file. Negative l_len values extend backwards
// from l_start; signed overflow and any resulting negative byte offset are
// rejected rather than wrapped into a different record range.
int normalizeRecordLockRange(const struct flock& lock,
                             off_t position,
                             wasmfs::File::Handle& lockedFile,
                             RecordLockRange& range) {
  off_t base;
  switch (lock.l_whence) {
    case SEEK_SET:
      base = 0;
      break;
    case SEEK_CUR:
      base = position;
      break;
    case SEEK_END:
      base = lockedFile.getSize();
      if (base < 0) {
        return base;
      }
      break;
    default:
      return -EINVAL;
  }

  if (addWouldOverflow(base, lock.l_start)) {
    return -EOVERFLOW;
  }
  off_t start = base + lock.l_start;
  if (start < 0) {
    return -EINVAL;
  }

  if (lock.l_len == 0) {
    range = {start, std::nullopt};
    return 0;
  }

  if (addWouldOverflow(start, lock.l_len)) {
    return -EOVERFLOW;
  }
  off_t end = start + lock.l_len;
  if (end < 0) {
    return -EINVAL;
  }

  if (end < start) {
    range = {end, start};
  } else {
    range = {start, end};
  }
  return 0;
}

bool isValidRecordLockType(short type, bool query) {
  if (query) {
    return type == F_RDLCK || type == F_WRLCK;
  }
  return type == F_RDLCK || type == F_WRLCK || type == F_UNLCK;
}

int checkRecordLockAccess(wasmfs::oflags_t flags, short type) {
  if (type == F_UNLCK) {
    return 0;
  }
  const auto accessMode = flags & O_ACCMODE;
  if ((type == F_RDLCK && accessMode == O_WRONLY) ||
      (type == F_WRLCK && accessMode == O_RDONLY)) {
    return -EBADF;
  }
  return 0;
}

int handleRecordLock(const std::shared_ptr<wasmfs::OpenFileState>& openFile,
                     struct flock* lock,
                     bool query) {
  if (!lock || !isValidRecordLockType(lock->l_type, query)) {
    return -EINVAL;
  }

  std::shared_ptr<wasmfs::File> file;
  off_t position;
  wasmfs::oflags_t flags;
  {
    auto lockedOpenFile = openFile->locked();
    file = lockedOpenFile.getFile();
    position = lockedOpenFile.getPosition();
    flags = lockedOpenFile.getFlags();
  }

  // Resolve the descriptor's real File before checking the backend. Pathname
  // symlinks have already been resolved by open(), and non-regular objects do
  // not obtain a synthetic lock service.
  if (!canUseRecordLocks(file)) {
    return -ENOTSUP;
  }

  auto lockedFile = file->locked();
  RecordLockRange range;
  if (int err = normalizeRecordLockRange(*lock, position, lockedFile, range)) {
    return err;
  }

  if (query) {
    // A valid leased OPFS profile admits exactly one WasmFS process. POSIX
    // F_GETLK ignores locks owned by that same process, so every supported
    // query is immediately unlocked. Linux returns F_UNLCK and leaves the
    // rest of the caller's flock unchanged when there is no conflict.
    lock->l_type = F_UNLCK;
    return 0;
  }

  if (int err = checkRecordLockAccess(flags, lock->l_type)) {
    return err;
  }

  // F_SETLKW is immediately satisfiable in this exclusive single-process
  // domain. It has the same state transition as F_SETLK and never blocks the
  // browser runtime thread waiting for a lock owner that cannot exist here.
  lockedFile.applyRecordLock(lock->l_type, range.start, range.end);
  return 0;
}

// WASI represents offsets as unsigned values, while WasmFS backends receive a
// signed off_t. Check a complete I/O vector before handing any part of it to a
// backend so `offset + length` cannot overflow there after an otherwise valid
// starting offset.
template <typename IOV>
bool fitsIOVRange(const IOV* iovs,
                  size_t iovs_len,
                  __wasi_filesize_t offset) {
  const auto maxOffset =
    static_cast<__wasi_filesize_t>(std::numeric_limits<off_t>::max());
  for (size_t i = 0; i < iovs_len; ++i) {
    auto length = static_cast<__wasi_filesize_t>(iovs[i].buf_len);
    if (length > maxOffset - offset) {
      return false;
    }
    offset += length;
  }
  return true;
}

// The POSIX vector APIs take a signed iovcnt, but the WASI ABI receives a
// size_t. Validate the converted count and the total result size before
// looking through the vector.
template <typename IOV>
__wasi_errno_t validateIOVs(const IOV* iovs, size_t iovs_len) {
  if (iovs_len > UIO_MAXIOV) {
    return __WASI_ERRNO_INVAL;
  }
  if (iovs_len && !iovs) {
    return __WASI_ERRNO_FAULT;
  }

  const auto maxBytes =
    static_cast<size_t>(std::numeric_limits<ssize_t>::max());
  size_t totalBytes = 0;
  for (size_t i = 0; i < iovs_len; ++i) {
    auto length = static_cast<size_t>(iovs[i].buf_len);
    if (length > maxBytes - totalBytes) {
      return __WASI_ERRNO_INVAL;
    }
    totalBytes += length;
  }
  return __WASI_ERRNO_SUCCESS;
}

} // anonymous namespace

extern "C" {

using namespace wasmfs;

// Every public WasmFS filesystem entrypoint below takes one of these guards.
// The guard is reentrant, so wrappers that delegate to another syscall remain
// one admitted operation while a terminal drain is waiting for them.
static __wasi_errno_t operationErrorToWasiErrno(int error) {
  // ESHUTDOWN is a POSIX extension (and has a value beyond the WASI errno
  // range). A terminal fence is best represented by WASI's valid CANCELED
  // result. The only other Operation error currently reachable by a public
  // entrypoint is a cross-instance reentrancy violation.
  switch (error) {
    case ESHUTDOWN:
      return __WASI_ERRNO_CANCELED;
    case EDEADLK:
      return __WASI_ERRNO_DEADLK;
    case EBUSY:
      return __WASI_ERRNO_BUSY;
    default:
      return __WASI_ERRNO_IO;
  }
}

static __wasi_errno_t admissionErrorToWasiErrno(int error) {
  assert(error < 0);
  return operationErrorToWasiErrno(-error);
}

static int admitFile(const std::shared_ptr<File>& file) {
  assert(file);
  return wasmFS.admitBackend(file->getBackend());
}

static int admitOpenFile(const std::shared_ptr<OpenFileState>& openFile) {
  if (!openFile) {
    return -EBADF;
  }
  return admitFile(openFile->locked().getFile());
}

#define WASMFS_GUARD_NEGATIVE()                                           \
  WasmFS::Operation wasmfsOperation(wasmFS);                              \
  if (!wasmfsOperation) {                                                  \
    return -wasmfsOperation.getError();                                    \
  }

#define WASMFS_GUARD_WASI()                                               \
  WasmFS::Operation wasmfsOperation(wasmFS);                              \
  if (!wasmfsOperation) {                                                  \
    return operationErrorToWasiErrno(wasmfsOperation.getError());          \
  }

// This is deliberately used only by __wasi_fd_write. During terminal
// fflush(NULL), musl reaches stdout/stderr through that entrypoint. The
// Operation implementation accepts it only on the draining thread and only
// while the terminal flush token is held; every other public WasmFS entrypoint
// remains fail-closed.
#define WASMFS_GUARD_WASI_STDIO_WRITE()                                    \
  WasmFS::Operation wasmfsOperation(                                      \
    wasmFS, WasmFS::Operation::Kind::StdioFlushWrite);                     \
  if (!wasmfsOperation) {                                                  \
    return operationErrorToWasiErrno(wasmfsOperation.getError());          \
  }

#define WASMFS_GUARD_BACKEND()                                            \
  WasmFS::Operation wasmfsOperation(wasmFS);                              \
  if (!wasmfsOperation) {                                                  \
    errno = wasmfsOperation.getError();                                    \
    return NullBackend;                                                    \
  }

int __syscall_dup3(int oldfd, int newfd, int flags) {
  WASMFS_GUARD_NEGATIVE();
  if (flags & ~O_CLOEXEC) {
    return -EINVAL;
  }
  if (oldfd == newfd) {
    return -EINVAL;
  }

  std::shared_ptr<DataFile> closee;
  {
    auto fileTable = wasmFS.getFileTable().locked();
    auto oldOpenFile = fileTable.getEntry(oldfd);
    if (!oldOpenFile) {
      return -EBADF;
    }
    if (int err = admitOpenFile(oldOpenFile)) {
      return err;
    }
    if (newfd < 0 || newfd >= WASMFS_FD_MAX) {
      return -EBADF;
    }

    if (auto replacedOpenFile = fileTable.getEntry(newfd)) {
      if (int err = admitOpenFile(replacedOpenFile)) {
        return err;
      }
    }

    // If the file descriptor newfd was previously open, it will just be
    // overwritten silently. If it was the last reference to its open file
    // state, close the data file after releasing the file table lock.
    closee = fileTable.setEntry(newfd, oldOpenFile);
  }
  if (closee) {
    // As with Linux dup2/dup3, errors from closing the overwritten descriptor
    // cannot be returned after the replacement succeeds.
    (void)closee->locked().close();
  }
  return newfd;
}

int __syscall_dup(int fd) {
  WASMFS_GUARD_NEGATIVE();
  auto fileTable = wasmFS.getFileTable().locked();

  // Check that an open file exists corresponding to the given fd.
  auto openFile = fileTable.getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  return fileTable.addEntry(openFile);
}

// This enum specifies whether file offset will be provided by the open file
// state or provided by argument in the case of pread or pwrite.
enum class OffsetHandling { OpenFileState, Argument };

// POSIX gives pread and pwrite a signed off_t, but the WASI ABI represents an
// offset as an unsigned filesize. Reject negative POSIX values converted at
// the ABI boundary, as well as any other value not representable by off_t.
static bool isValidOffset(__wasi_filesize_t offset) {
  return offset <=
         static_cast<__wasi_filesize_t>(std::numeric_limits<off_t>::max());
}

// Internal write function called by __wasi_fd_write and __wasi_fd_pwrite
// Receives an open file state offset.
// Optionally sets open file state offset.
static __wasi_errno_t writeAtOffset(OffsetHandling setOffset,
                                    __wasi_fd_t fd,
                                    const __wasi_ciovec_t* iovs,
                                    size_t iovs_len,
                                    __wasi_size_t* nwritten,
                                    __wasi_filesize_t offset = 0) {
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return __WASI_ERRNO_BADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return admissionErrorToWasiErrno(err);
  }

  if (!isValidOffset(offset)) {
    return __WASI_ERRNO_INVAL;
  }

  auto lockedOpenFile = openFile->locked();

  auto file = lockedOpenFile.getFile();
  // Positioned I/O needs a seekable file even when the open descriptor's
  // access mode would independently reject the operation.
  if (setOffset == OffsetHandling::Argument && !file->isSeekable()) {
    return __WASI_ERRNO_SPIPE;
  }

  // A file opened for reading only (O_RDONLY) cannot be written. POSIX write(2)
  // returns EBADF when the file descriptor is not open for writing.
  if ((lockedOpenFile.getFlags() & O_ACCMODE) == O_RDONLY) {
    return __WASI_ERRNO_BADF;
  }

  if (auto err = validateIOVs(iovs, iovs_len)) {
    return err;
  }
  if (iovs_len == 0) {
    *nwritten = 0;
    return __WASI_ERRNO_SUCCESS;
  }

  auto dataFile = file->dynCast<DataFile>();
  if (!dataFile) {
    return __WASI_ERRNO_ISDIR;
  }

  if (setOffset == OffsetHandling::Argument &&
      !fitsIOVRange(iovs, iovs_len, offset)) {
    return __WASI_ERRNO_INVAL;
  }

  auto lockedFile = dataFile->locked();
  const bool atomicMetadataMutation =
    requiresAtomicMetadataMutations(dataFile);

  if (setOffset == OffsetHandling::OpenFileState) {
    if (lockedOpenFile.getFlags() & O_APPEND) {
      off_t size = lockedFile.getSize();
      if (size < 0) {
        // Translate to WASI standard of positive return codes.
        return -size;
      }
      offset = size;
    } else {
      offset = lockedOpenFile.getPosition();
    }

    if (!isValidOffset(offset) || !fitsIOVRange(iovs, iovs_len, offset)) {
      return __WASI_ERRNO_INVAL;
    }

    if (lockedOpenFile.getFlags() & O_APPEND) {
      lockedOpenFile.setPosition(offset);
    }
  }

  size_t bytesWritten = 0;
  for (size_t i = 0; i < iovs_len; i++) {
    off_t len = iovs[i].buf_len;

    // A zero-length iovec is a no-op and must not invoke a backend write. In
    // particular, a write must not inspect or extend a file at its offset.
    if (len == 0) {
      continue;
    }

    const uint8_t* buf = iovs[i].buf;

    // Check if buf_len specifies a positive length buffer but buf is a
    // null pointer
    if (!buf && len > 0) {
      return __WASI_ERRNO_INVAL;
    }

    // Check if the sum of the buf_len values overflows an off_t (63 bits).
    if (addWillOverFlow(offset, (__wasi_filesize_t)bytesWritten)) {
      return __WASI_ERRNO_FBIG;
    }

    // An opted-in backend must commit the bytes and their metadata post-image
    // together. In particular, do not retry through write() if its paired hook
    // reports ENOTSUP or a real storage failure.
    auto result = atomicMetadataMutation
                    ? lockedFile.writeWithMetadata(
                        buf,
                        len,
                        offset + bytesWritten,
                        dataMutationMetadataPostImage(lockedFile))
                    : lockedFile.write(buf, len, offset + bytesWritten);
    if (result < 0) {
      // This individual write failed. Report the error unless we've already
      // written some bytes, in which case report a successful short write.
      if (bytesWritten > 0) {
        break;
      }
      return -result;
    }
    // The write was successful.
    bytesWritten += result;
    if (result < len) {
      // The write was short, so stop here.
      break;
    }
  }
  *nwritten = bytesWritten;
  if (setOffset == OffsetHandling::OpenFileState &&
      lockedOpenFile.getFile()->isSeekable()) {
    lockedOpenFile.setPosition(offset + bytesWritten);
  }
  if (bytesWritten && !atomicMetadataMutation) {
    lockedFile.updateMTime();
  }
  return __WASI_ERRNO_SUCCESS;
}

// Internal read function called by __wasi_fd_read and __wasi_fd_pread
// Receives an open file state offset.
// Optionally sets open file state offset.
// TODO: combine this with writeAtOffset because the code is nearly identical.
static __wasi_errno_t readAtOffset(OffsetHandling setOffset,
                                   __wasi_fd_t fd,
                                   const __wasi_iovec_t* iovs,
                                   size_t iovs_len,
                                   __wasi_size_t* nread,
                                   __wasi_filesize_t offset = 0) {
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return __WASI_ERRNO_BADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return admissionErrorToWasiErrno(err);
  }

  if (setOffset == OffsetHandling::Argument && !isValidOffset(offset)) {
    return __WASI_ERRNO_INVAL;
  }

  auto lockedOpenFile = openFile->locked();

  if (setOffset == OffsetHandling::OpenFileState) {
    offset = lockedOpenFile.getPosition();
    if (!isValidOffset(offset)) {
      return __WASI_ERRNO_INVAL;
    }
  }

  auto file = lockedOpenFile.getFile();
  // Positioned I/O needs a seekable file even when the open descriptor's
  // access mode would independently reject the operation.
  if (setOffset == OffsetHandling::Argument && !file->isSeekable()) {
    return __WASI_ERRNO_SPIPE;
  }

  // A file opened for writing only (O_WRONLY) cannot be read. POSIX read(2)
  // returns EBADF when the file descriptor is not open for reading.
  if ((lockedOpenFile.getFlags() & O_ACCMODE) == O_WRONLY) {
    return __WASI_ERRNO_BADF;
  }

  if (auto err = validateIOVs(iovs, iovs_len)) {
    return err;
  }
  if (iovs_len == 0) {
    *nread = 0;
    return __WASI_ERRNO_SUCCESS;
  }

  auto dataFile = file->dynCast<DataFile>();

  // If file is nullptr, then the file was not a DataFile.
  if (!dataFile) {
    return __WASI_ERRNO_ISDIR;
  }

  if (!fitsIOVRange(iovs, iovs_len, offset)) {
    return __WASI_ERRNO_INVAL;
  }

  auto lockedFile = dataFile->locked();

  size_t bytesRead = 0;
  for (size_t i = 0; i < iovs_len; i++) {
    size_t len = iovs[i].buf_len;

    // A zero-length iovec is a no-op and must not invoke a backend read. In
    // particular, a read must not inspect its offset or buffer.
    if (len == 0) {
      continue;
    }

    uint8_t* buf = iovs[i].buf;

    if (!buf && len > 0) {
      return __WASI_ERRNO_INVAL;
    }

    auto result = lockedFile.read(buf, len, offset + bytesRead);
    if (result < 0) {
      // This individual read failed. Report the error unless we've already read
      // some bytes, in which case report a successful short read.
      if (bytesRead > 0) {
        break;
      }
      return -result;
    }

    // The read was successful.

    // Backends must only return len or less.
    assert(result <= len);

    bytesRead += result;
    if (result < len) {
      // The read was short, so stop here.
      break;
    }
  }
  *nread = bytesRead;
  if (setOffset == OffsetHandling::OpenFileState &&
      lockedOpenFile.getFile()->isSeekable()) {
    lockedOpenFile.setPosition(offset + bytesRead);
  }
  return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_write(__wasi_fd_t fd,
                               const __wasi_ciovec_t* iovs,
                               size_t iovs_len,
                               __wasi_size_t* nwritten) {
  WASMFS_GUARD_WASI_STDIO_WRITE();
  return writeAtOffset(
    OffsetHandling::OpenFileState, fd, iovs, iovs_len, nwritten);
}

__wasi_errno_t __wasi_fd_read(__wasi_fd_t fd,
                              const __wasi_iovec_t* iovs,
                              size_t iovs_len,
                              __wasi_size_t* nread) {
  WASMFS_GUARD_WASI();
  return readAtOffset(OffsetHandling::OpenFileState, fd, iovs, iovs_len, nread);
}

__wasi_errno_t __wasi_fd_pwrite(__wasi_fd_t fd,
                                const __wasi_ciovec_t* iovs,
                                size_t iovs_len,
                                __wasi_filesize_t offset,
                                __wasi_size_t* nwritten) {
  WASMFS_GUARD_WASI();
  return writeAtOffset(
    OffsetHandling::Argument, fd, iovs, iovs_len, nwritten, offset);
}

__wasi_errno_t __wasi_fd_pread(__wasi_fd_t fd,
                               const __wasi_iovec_t* iovs,
                               size_t iovs_len,
                               __wasi_filesize_t offset,
                               __wasi_size_t* nread) {
  WASMFS_GUARD_WASI();
  return readAtOffset(
    OffsetHandling::Argument, fd, iovs, iovs_len, nread, offset);
}

__wasi_errno_t __wasi_fd_close(__wasi_fd_t fd) {
  WASMFS_GUARD_WASI();
  std::shared_ptr<DataFile> closee;
  {
    // Do not hold the file table lock while performing the close.
    auto fileTable = wasmFS.getFileTable().locked();
    auto entry = fileTable.getEntry(fd);
    if (!entry) {
      return __WASI_ERRNO_BADF;
    }
    if (int err = admitOpenFile(entry)) {
      return admissionErrorToWasiErrno(err);
    }
    closee = fileTable.setEntry(fd, nullptr);
  }
  if (closee) {
    // Translate to WASI standard of positive return codes.
    int ret = -closee->locked().close();
    assert(ret >= 0);
    return ret;
  }
  return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_sync(__wasi_fd_t fd) {
  WASMFS_GUARD_WASI();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return __WASI_ERRNO_BADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return admissionErrorToWasiErrno(err);
  }

  auto file = openFile->locked().getFile();
  auto dataFile = file->dynCast<DataFile>();
  if (dataFile) {
    auto ret = dataFile->locked().flush();
    assert(ret <= 0);
    // Translate to WASI standard of positive return codes.
    return -ret;
  }

  if (auto directory = file->dynCast<Directory>()) {
    auto ret = directory->locked().flush();
    assert(ret <= 0);
    // Translate to WASI standard of positive return codes.
    return -ret;
  }

  return __WASI_ERRNO_SUCCESS;
}

int __syscall_fdatasync(int fd) {
  WASMFS_GUARD_NEGATIVE();
  // TODO: Optimize this to avoid unnecessarily flushing unnecessary metadata.
  // `__syscall_*` functions use negative errno values, while the WASI entry
  // point returns a positive WASI errno.
  return -__wasi_fd_sync(fd);
}

wasmfs::backend_t wasmfs_get_backend_by_fd(int fd) {
  WASMFS_GUARD_BACKEND();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return NullBackend;
  }
  if (int err = admitOpenFile(openFile)) {
    errno = -err;
    return NullBackend;
  }
  return openFile->locked().getFile()->getBackend();
}

// This function is exposed to users to allow them to obtain a backend_t for a
// specified path.
wasmfs::backend_t wasmfs_get_backend_by_path(const char* path) {
  WASMFS_GUARD_BACKEND();
  auto parsed = path::parseFile(path);
  if (auto err = parsed.getError()) {
    // Preserve a sealed profile's admission failure instead of making it look
    // like an ordinary absent path.
    errno = -err;
    return NullBackend;
  }
  return parsed.getFile()->getBackend();
}

static timespec ms_to_timespec(double ms) {
  long long seconds = ms / 1000;
  timespec ts;
  ts.tv_sec = seconds; // seconds
  ts.tv_nsec = (ms - (seconds * 1000)) * 1000 * 1000; // nanoseconds
  return ts;
}

int __syscall_newfstatat(int dirfd, intptr_t path, intptr_t buf, int flags) {
  WASMFS_GUARD_NEGATIVE();
  // Only accept valid flags.
  if (flags & ~(AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW)) {
    // TODO: Test this case.
    return -EINVAL;
  }
  auto parsed = path::getFileAt(dirfd, (char*)path, flags);
  if (auto err = parsed.getError()) {
    return err;
  }
  auto file = parsed.getFile();

  // Extract the information from the file.
  auto lockedFile = file->locked();
  auto buffer = (struct stat*)buf;

  off_t size = lockedFile.getSize();
  if (size < 0) {
    return size;
  }
  buffer->st_size = size;

  // ATTN: hard-coded constant values are copied from the existing JS file
  // system. Specific values were chosen to match existing library_fs.js
  // values.
  // ID of device containing file: Hardcode 1 for now, no meaning at the
  // moment for Emscripten.
  buffer->st_dev = 1;
  buffer->st_mode = lockedFile.getMode();
  buffer->st_ino = file->getIno();
  // The number of hard links is 1 since they are unsupported.
  buffer->st_nlink = 1;
  buffer->st_uid = 0;
  buffer->st_gid = 0;
  // Device ID (if special file) No meaning right now for Emscripten.
  buffer->st_rdev = 0;
  // The syscall docs state this is hardcoded to # of 512 byte blocks.
  buffer->st_blocks = (buffer->st_size + 511) / 512;
  // Specifies the preferred blocksize for efficient disk I/O.
  buffer->st_blksize = 4096;
  buffer->st_atim = ms_to_timespec(lockedFile.getATime());
  buffer->st_mtim = ms_to_timespec(lockedFile.getMTime());
  buffer->st_ctim = ms_to_timespec(lockedFile.getCTime());
  return __WASI_ERRNO_SUCCESS;
}

int __syscall_stat64(intptr_t path, intptr_t buf) {
  WASMFS_GUARD_NEGATIVE();
  return __syscall_newfstatat(AT_FDCWD, path, buf, 0);
}

int __syscall_lstat64(intptr_t path, intptr_t buf) {
  WASMFS_GUARD_NEGATIVE();
  return __syscall_newfstatat(AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
}

int __syscall_fstat64(int fd, intptr_t buf) {
  WASMFS_GUARD_NEGATIVE();
  return __syscall_newfstatat(fd, (intptr_t) "", buf, AT_EMPTY_PATH);
}

// When calling doOpen(), we may request an FD be returned, or we may not need
// that return value (in which case no FD need be allocated, and we return 0 on
// success).
enum class OpenReturnMode { FD, Nothing };

static int validateOpenFlags(int flags) {
  int accessMode = (flags & O_ACCMODE);
  if (accessMode != O_WRONLY && accessMode != O_RDONLY &&
      accessMode != O_RDWR) {
    return -EINVAL;
  }

  // WasmFS does not provide a per-write durability contract for these flags.
  // Reject them rather than report a successful write that was not synced.
  // O_RSYNC is an alias of O_SYNC on Emscripten and is caught here as well.
  if (flags & (O_SYNC | O_DSYNC)) {
    return -ENOTSUP;
  }

  // O_TMPFILE includes O_DIRECTORY, which is otherwise supported. Reject only
  // the full temporary-file operation; its private bit alone remains invalid.
  if ((flags & O_TMPFILE) == O_TMPFILE) {
    return -ENOTSUP;
  }

  constexpr int unsupportedFlags = O_DIRECT | O_ASYNC | O_NOATIME;
  if (flags & unsupportedFlags) {
    return -ENOTSUP;
  }

  // O_NOCTTY is a harmless no-op because WasmFS has no controlling terminal.
  constexpr int allowedFlags =
    O_CREAT | O_EXCL | O_DIRECTORY | O_TRUNC | O_APPEND | O_RDWR | O_WRONLY |
    O_RDONLY | O_LARGEFILE | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY;
  if (flags & ~allowedFlags) {
    return -EINVAL;
  }

  return 0;
}

// An OpenFileState opens its DataFile before it can be installed in the file
// table. If installing it fails, close that physical open before discarding the
// state. In particular, an OPFS file must not retain a SyncAccessHandle merely
// because the WasmFS descriptor table is full.
static int installOpenFile(std::shared_ptr<OpenFileState> openFile) {
  int fd;
  {
    auto fileTable = wasmFS.getFileTable().locked();
    fd = fileTable.addEntry(openFile);
  }
  if (fd >= 0) {
    return fd;
  }

  auto dataFile = openFile->locked().getFile()->dynCast<DataFile>();
  if (!dataFile) {
    return fd;
  }
  if (int err = dataFile->locked().close()) {
    return err;
  }
  return fd;
}

// Close a newly opened state before returning an error from a later part of
// open(). The state has never entered the file table, so its physical open
// cannot otherwise be reached by fd_close.
static int abandonOpenFile(std::shared_ptr<OpenFileState> openFile) {
  auto dataFile = openFile->locked().getFile()->dynCast<DataFile>();
  if (!dataFile) {
    return 0;
  }
  return dataFile->locked().close();
}

static __wasi_fd_t doOpen(path::ParsedParent parsed,
                          int flags,
                          mode_t mode,
                          wasmfs::backend_t backend = NullBackend,
                          OpenReturnMode returnMode = OpenReturnMode::FD) {
  if (auto err = validateOpenFlags(flags)) {
    return err;
  }

  int accessMode = (flags & O_ACCMODE);

  if (auto err = parsed.getError()) {
    return err;
  }
  auto& [parent, childName] = parsed.getParentChild();
  if (childName.size() > WASMFS_NAME_MAX) {
    return -ENAMETOOLONG;
  }

  std::shared_ptr<File> child;
  {
    auto lockedParent = parent->locked();
    auto lookup = lockedParent.getChildWithError(std::string(childName));
    if (int err = lookup.getError()) {
      return err;
    }
    child = lookup.getFile();
    // The requested node was not found.
    if (!child) {
      // If curr is the last element and the create flag is specified
      // If O_DIRECTORY is also specified, still create a regular file:
      // https://man7.org/linux/man-pages/man2/open.2.html#BUGS
      if (!(flags & O_CREAT)) {
        return -ENOENT;
      }

      // Inserting into an unlinked directory is not allowed.
      if (!lockedParent.getParent()) {
        return -ENOENT;
      }

      // Creating a child modifies the parent directory, so it requires write
      // permission on that directory.
      if (!(lockedParent.getMode() & WASMFS_PERM_WRITE)) {
        return -EACCES;
      }

      // Mask out everything except the permissions bits.
      mode &= S_IALLUGO;

      // If there is no explicitly provided backend, use the parent's backend.
      if (!backend) {
        backend = parent->getBackend();
      }
      // A factory-selected target backend may be distinct from the pathname
      // parent. Admit it before creation so a sealed leased profile cannot be
      // remounted through this explicit-backend API.
      if (int err = wasmFS.admitBackend(backend)) {
        return err;
      }

      std::shared_ptr<File> created;
      if (backend == parent->getBackend()) {
        created = lockedParent.insertDataFile(std::string(childName), mode);
        if (!created) {
          // TODO Receive a specific error code, and report it here. For now,
          //      report a generic error.
          return -EIO;
        }
      } else {
        created = backend->createFile(mode);
        if (!created) {
          // TODO Receive a specific error code, and report it here. For now,
          //      report a generic error.
          return -EIO;
        }
        [[maybe_unused]] bool mounted =
          lockedParent.mountChild(std::string(childName), created);
        assert(mounted);
      }
      // TODO: Check that the insert actually succeeds.
      if (returnMode == OpenReturnMode::Nothing) {
        return 0;
      }

      std::shared_ptr<OpenFileState> openFile;
      if (auto err = OpenFileState::create(created, flags, openFile)) {
        assert(err < 0);
        return err;
      }
      return installOpenFile(std::move(openFile));
    }
  }

  // Path traversal admitted the parent, but a mount may yield a child from a
  // different backend. Hold that exact backend for all leaf inspection and
  // open/truncate work below.
  if (int err = admitFile(child)) {
    return err;
  }

  if (auto link = child->dynCast<Symlink>()) {
    if (flags & O_NOFOLLOW) {
      return -ELOOP;
    }
    // TODO: The link dereference count starts back at 0 here. We could
    // propagate it from the previous path parsing instead.
    auto target = link->getTarget();
    auto parsedLink = path::getFileFrom(parent, target);
    if (auto err = parsedLink.getError()) {
      return err;
    }
    child = parsedLink.getFile();
  }
  assert(!child->is<Symlink>());

  // Return an error if the file exists and O_CREAT and O_EXCL are specified.
  if ((flags & O_EXCL) && (flags & O_CREAT)) {
    return -EEXIST;
  }

  if (child->is<Directory>() && (accessMode != O_RDONLY || (flags & O_CREAT))) {
    return -EISDIR;
  }

  // Check user permissions.
  auto fileMode = child->locked().getMode();
  if ((accessMode == O_RDONLY || accessMode == O_RDWR) &&
      !(fileMode & WASMFS_PERM_READ)) {
    return -EACCES;
  }
  if ((accessMode == O_WRONLY || accessMode == O_RDWR) &&
      !(fileMode & WASMFS_PERM_WRITE)) {
    return -EACCES;
  }

  // Fail if O_DIRECTORY is specified and pathname is not a directory
  if (flags & O_DIRECTORY && !child->is<Directory>()) {
    return -ENOTDIR;
  }

  // Note that we open the file before truncating it because some backends may
  // truncate opened files more efficiently (e.g. OPFS).
  std::shared_ptr<OpenFileState> openFile;
  if (auto err = OpenFileState::create(child, flags, openFile)) {
    assert(err < 0);
    return err;
  }

  // If O_TRUNC, truncate the file if possible.
  if (flags & O_TRUNC) {
    if (!child->is<DataFile>()) {
      if (int err = abandonOpenFile(std::move(openFile))) {
        return err;
      }
      return -EISDIR;
    }
    if ((fileMode & WASMFS_PERM_WRITE) == 0) {
      if (int err = abandonOpenFile(std::move(openFile))) {
        return err;
      }
      return -EACCES;
    }
    auto dataFile = child->cast<DataFile>();
    const bool atomicMetadataMutation =
      requiresAtomicMetadataMutations(dataFile);
    auto lockedDataFile = dataFile->locked();
    int truncateErr = resizeDataFile(dataFile, lockedDataFile, 0);
    // Preserve the historical O_RDONLY | O_TRUNC behavior. In particular,
    // OPFS may use a non-truncatable Blob for a read-only open. An opted-in
    // atomic backend is different: accepting a failed paired mutation would
    // falsely claim that its content/metadata transaction completed.
    if (truncateErr &&
        (accessMode != O_RDONLY || atomicMetadataMutation)) {
      // The state has not entered the file table. Release its physical open
      // before reporting the truncation failure, and prefer a cleanup failure
      // because it may leave a browser-side handle live.
      if (int closeErr = abandonOpenFile(std::move(openFile))) {
        return closeErr;
      }
      return truncateErr;
    }
  }

  return installOpenFile(std::move(openFile));
}

// This function is exposed to users and allows users to create a file in a
// specific backend. An fd to an open file is returned.
int wasmfs_create_file(char* pathname,
                       mode_t mode,
                       wasmfs::backend_t backend) {
  WASMFS_GUARD_NEGATIVE();
  static_assert(std::is_same_v<decltype(doOpen(0, 0, 0, 0)), unsigned int>,
                "unexpected conversion from result of doOpen to int");
  return doOpen(path::parseParent((char*)pathname),
                O_CREAT | O_EXCL | O_RDWR,
                mode,
                backend);
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_openat(int dirfd, intptr_t path, int flags, ...) {
  WASMFS_GUARD_NEGATIVE();
  mode_t mode = 0;
  va_list v1;
  va_start(v1, flags);
  mode = va_arg(v1, int);
  va_end(v1);

  // Validate before resolving the path, so unsupported O_CREAT combinations
  // cannot create a file as a side effect.
  if (auto err = validateOpenFlags(flags)) {
    return err;
  }

  return doOpen(path::parseParent((char*)path, dirfd), flags, mode);
}

int __syscall_mknodat(int dirfd, intptr_t path, int mode, int dev) {
  WASMFS_GUARD_NEGATIVE();
  // Validate the requested node type before resolving the path. WasmFS can
  // create regular files, but has no implementations for special nodes.
  switch (mode & S_IFMT) {
    case 0:
    case S_IFREG:
      break;
    case S_IFCHR:
    case S_IFBLK:
    case S_IFIFO:
    case S_IFSOCK:
      return -ENOTSUP;
    case S_IFDIR:
    case S_IFLNK:
    default:
      return -EINVAL;
  }

  // Device numbers apply only to special node types, which WasmFS rejects.
  (void)dev;
  return doOpen(path::parseParent((char*)path, dirfd),
                O_CREAT | O_EXCL,
                mode,
                NullBackend,
                OpenReturnMode::Nothing);
}

static int
doMkdir(path::ParsedParent parsed,
        int mode,
        wasmfs::backend_t backend = NullBackend) {
  if (auto err = parsed.getError()) {
    return err;
  }
  auto& [parent, childNameView] = parsed.getParentChild();
  std::string childName(childNameView);
  auto lockedParent = parent->locked();

  if (childName.size() > WASMFS_NAME_MAX) {
    return -ENAMETOOLONG;
  }

  // Check if the requested directory already exists.
  auto lookup = lockedParent.getChildWithError(childName);
  if (int err = lookup.getError()) {
    return err;
  }
  if (auto child = lookup.getFile()) {
    if (int err = admitFile(child)) {
      return err;
    }
    return -EEXIST;
  }

  // Mask rwx permissions for user, group and others, and the sticky bit.
  // This prevents users from entering S_IFREG for example.
  // https://www.gnu.org/software/libc/manual/html_node/Permission-Bits.html
  mode &= S_IRWXUGO | S_ISVTX;

  if (!(lockedParent.getMode() & WASMFS_PERM_WRITE)) {
    return -EACCES;
  }

  // By default, the backend that the directory is created in is the same as
  // the parent directory. However, if a backend is passed as a parameter,
  // then that backend is used.
  if (!backend) {
    backend = parent->getBackend();
  }
  // An explicitly selected backend can differ from the parent mount. Check
  // it before calling its factory so sealing cannot be bypassed by a new
  // mountpoint or directory.
  if (int err = wasmFS.admitBackend(backend)) {
    return err;
  }

  if (backend == parent->getBackend()) {
    if (!lockedParent.insertDirectory(childName, mode)) {
      // TODO Receive a specific error code, and report it here. For now, report
      //      a generic error.
      return -EIO;
    }
  } else {
    auto created = backend->createDirectory(mode);
    if (!created) {
      // TODO Receive a specific error code, and report it here. For now, report
      //      a generic error.
      return -EIO;
    }
    [[maybe_unused]] bool mounted = lockedParent.mountChild(childName, created);
    assert(mounted);
  }

  // TODO: Check that the insertion is successful.

  return 0;
}

// This function is exposed to users and allows users to specify a particular
// backend that a directory should be created within.
int wasmfs_create_directory(char* path,
                            int mode,
                            wasmfs::backend_t backend) {
  WASMFS_GUARD_NEGATIVE();
  static_assert(std::is_same_v<decltype(doMkdir(0, 0, 0)), int>,
                "unexpected conversion from result of doMkdir to int");
  return doMkdir(path::parseParent(path), mode, backend);
}

// TODO: Test this.
int __syscall_mkdirat(int dirfd, intptr_t path, int mode) {
  WASMFS_GUARD_NEGATIVE();
  return doMkdir(path::parseParent((char*)path, dirfd), mode);
}

// The JavaScript-facing WasmFS bridge returns an f64, so it must not change a
// descriptor position outside JavaScript's exact-integer i53 range.
constexpr off_t JSExactOffsetLimit = INT64_C(1) << 53;

static __wasi_errno_t doFdSeek(__wasi_fd_t fd,
                               __wasi_filedelta_t offset,
                               __wasi_whence_t whence,
                               __wasi_filesize_t* newoffset,
                               off_t maxPosition) {
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return __WASI_ERRNO_BADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return admissionErrorToWasiErrno(err);
  }
  auto lockedOpenFile = openFile->locked();

  if (!lockedOpenFile.getFile()->isSeekable()) {
    return __WASI_ERRNO_SPIPE;
  }

  off_t position;
  if (whence == SEEK_SET) {
    position = offset;
  } else if (whence == SEEK_CUR) {
    if (__builtin_add_overflow(
          lockedOpenFile.getPosition(), offset, &position)) {
      return __WASI_ERRNO_INVAL;
    }
  } else if (whence == SEEK_END) {
    // Only the open file state is altered in seek. Locking the underlying
    // data file here once is sufficient.
    off_t size = lockedOpenFile.getFile()->locked().getSize();
    if (size < 0) {
      // Translate to WASI standard of positive return codes.
      return -size;
    }
    if (__builtin_add_overflow(size, offset, &position)) {
      return __WASI_ERRNO_INVAL;
    }
  } else {
    return __WASI_ERRNO_INVAL;
  }

  if (position < 0) {
    return __WASI_ERRNO_INVAL;
  }
  if (position > maxPosition) {
    return __WASI_ERRNO_OVERFLOW;
  }

  lockedOpenFile.setPosition(position);

  if (newoffset) {
    *newoffset = position;
  }

  return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t __wasi_fd_seek(__wasi_fd_t fd,
                              __wasi_filedelta_t offset,
                              __wasi_whence_t whence,
                              __wasi_filesize_t* newoffset) {
  WASMFS_GUARD_WASI();
  return doFdSeek(
    fd, offset, whence, newoffset, std::numeric_limits<off_t>::max());
}

// This private variant preserves the JS bridge's exact-integer contract
// without constraining native POSIX or WASI callers.
__wasi_errno_t __wasmfs_fd_seek_for_js(__wasi_fd_t fd,
                                       __wasi_filedelta_t offset,
                                       __wasi_whence_t whence,
                                       __wasi_filesize_t* newoffset) {
  WASMFS_GUARD_WASI();
  return doFdSeek(fd, offset, whence, newoffset, JSExactOffsetLimit);
}

static int doChdir(WasmFS::CWDTransition& cwdTransition,
                   std::shared_ptr<File>& file) {
  auto dir = file->dynCast<Directory>();
  if (!dir) {
    return -ENOTDIR;
  }
  cwdTransition.setCWD(dir);
  return 0;
}

int __syscall_chdir(intptr_t path) {
  WASMFS_GUARD_NEGATIVE();
  auto cwdTransition = wasmFS.beginCWDTransition();
  auto parsed = path::parseFile((char*)path);
  if (auto err = parsed.getError()) {
    return err;
  }
  return doChdir(cwdTransition, parsed.getFile());
}

int __syscall_fchdir(int fd) {
  WASMFS_GUARD_NEGATIVE();
  auto cwdTransition = wasmFS.beginCWDTransition();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  std::shared_ptr<File> file;
  {
    auto lockedOpenFile = openFile->locked();
    file = lockedOpenFile.getFile();
  }
  if (int err = admitFile(file)) {
    return err;
  }
  return doChdir(cwdTransition, file);
}

int __syscall_getcwd(intptr_t buf, size_t size) {
  WASMFS_GUARD_NEGATIVE();
  // Check if buf points to a bad address.
  if (!buf && size > 0) {
    return -EFAULT;
  }

  // Check if the size argument is zero and buf is not a null pointer.
  if (buf && size == 0) {
    return -EINVAL;
  }

  auto curr = wasmFS.getCWD();
  // Even when CWD is the root, it can be a sealed leased-OPFS directory.
  // Admit it before the ancestry loop so getcwd() cannot fabricate "/" after
  // a scoped profile drain.
  if (int err = admitFile(curr)) {
    return err;
  }

  std::string result = "";

  while (curr != wasmFS.getRootDirectory()) {
    if (int err = admitFile(curr)) {
      return err;
    }
    auto parent = curr->locked().getParent();
    // Check if the parent exists. The parent may not exist if the CWD or one
    // of its ancestors has been unlinked.
    if (!parent) {
      return -ENOENT;
    }

    auto name = parent->locked().getName(curr);
    result = '/' + name + result;
    curr = parent;
  }

  // Check if the cwd is the root directory.
  if (result.empty()) {
    result = "/";
  }

  int len = result.length() + 1;

  // Check if the size argument is less than the length of the absolute
  // pathname of the working directory, including null terminator.
  if (len > size) {
    return -ERANGE;
  }

  // Return value is a null-terminated c string.
  strcpy((char*)buf, result.c_str());

  return len;
}

__wasi_errno_t __wasi_fd_fdstat_get(__wasi_fd_t fd, __wasi_fdstat_t* stat) {
  WASMFS_GUARD_WASI();
  // TODO: This is only partial implementation of __wasi_fd_fdstat_get. Enough
  // to get __wasi_fd_is_valid working.
  // There are other fields in the stat structure that we should really
  // be filling in here.
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return __WASI_ERRNO_BADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return admissionErrorToWasiErrno(err);
  }

  if (openFile->locked().getFile()->is<Directory>()) {
    stat->fs_filetype = __WASI_FILETYPE_DIRECTORY;
  } else {
    stat->fs_filetype = __WASI_FILETYPE_REGULAR_FILE;
  }
  return __WASI_ERRNO_SUCCESS;
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_unlinkat(int dirfd, intptr_t path, int flags) {
  WASMFS_GUARD_NEGATIVE();
  if (flags & ~AT_REMOVEDIR) {
    // TODO: Test this case.
    return -EINVAL;
  }
  // It is invalid for rmdir paths to end in ".", but we need to distinguish
  // this case from the case of `parseParent` returning (root, '.') when parsing
  // "/", so we need to find the invalid "/." manually.
  if (flags == AT_REMOVEDIR) {
    std::string_view p((char*)path);
    // Ignore trailing '/'.
    while (!p.empty() && p.back() == '/') {
      p.remove_suffix(1);
    }
    if (p.size() >= 2 && p.substr(p.size() - 2) == std::string_view("/.")) {
      return -EINVAL;
    }
  }
  auto parsed = path::parseParent((char*)path, dirfd);
  if (auto err = parsed.getError()) {
    return err;
  }
  auto& [parent, childNameView] = parsed.getParentChild();
  std::string childName(childNameView);
  auto lockedParent = parent->locked();
  auto lookup = lockedParent.getChildWithError(childName);
  if (int err = lookup.getError()) {
    return err;
  }
  auto file = lookup.getFile();
  if (!file) {
    return -ENOENT;
  }
  if (int err = admitFile(file)) {
    return err;
  }
  // Disallow removing the root directory, even if it is empty.
  if (file == wasmFS.getRootDirectory()) {
    return -EBUSY;
  }

  auto lockedFile = file->locked();
  if (auto dir = file->dynCast<Directory>()) {
    if (flags != AT_REMOVEDIR) {
      return -EISDIR;
    }
    // Generic rmdir must not detach a mountpoint. wasmfs_unmount() is the
    // supported direct detach operation and serializes it with CWD transitions.
    if (parent->getBackend() != dir->getBackend()) {
      return -EBUSY;
    }
    // A directory can only be removed if it has no entries.
    auto numEntries = dir->locked().getNumEntries();
    if (numEntries < 0) {
      return numEntries;
    }
    if (numEntries > 0) {
      return -ENOTEMPTY;
    }
  } else {
    // A normal file or symlink.
    if (flags == AT_REMOVEDIR) {
      return -ENOTDIR;
    }
  }

  // Cannot unlink/rmdir if the parent dir doesn't have write permissions.
  if (!(lockedParent.getMode() & WASMFS_PERM_WRITE)) {
    return -EACCES;
  }

  // Input is valid, perform the unlink.
  return lockedParent.removeChild(childName);
}

int __syscall_rmdir(intptr_t path) {
  WASMFS_GUARD_NEGATIVE();
  return __syscall_unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

// wasmfs_unmount is similar to __syscall_unlinkat, but assumes AT_REMOVEDIR is
// true and will only unlink mountpoints (Empty and nonempty).
int wasmfs_unmount(const char* path) {
  WASMFS_GUARD_NEGATIVE();
  auto cwdTransition = wasmFS.beginCWDTransition();
  auto parsed = path::parseParent(path, AT_FDCWD);
  if (auto err = parsed.getError()) {
    return err;
  }
  auto& [parent, childNameView] = parsed.getParentChild();
  std::string childName(childNameView);

  // Do not hold the mount's parent lock while walking CWD ancestry. Apart from
  // avoiding nested directory locks, revalidate the mountpoint before unlinking
  // in case another thread changes it while the CWD check is in progress.
  while (true) {
    std::shared_ptr<Directory> mount;
    {
      auto lockedParent = parent->locked();
      auto lookup = lockedParent.getChildWithError(childName);
      if (int err = lookup.getError()) {
        return err;
      }
      auto file = lookup.getFile();
      if (!file) {
        return -ENOENT;
      }
      // Preserve sealed-backend precedence even when the leaf is not a
      // directory/mountpoint. Otherwise unmount("/profile/file") could turn
      // a profile admission failure into an invented ENOTDIR.
      if (int err = admitFile(file)) {
        return err;
      }
      // Disallow removing the root directory, even if it is empty.
      if (file == wasmFS.getRootDirectory()) {
        return -EBUSY;
      }

      mount = file->dynCast<Directory>();
      if (!mount) {
        // A normal file or symlink.
        return -ENOTDIR;
      }

      if (parent->getBackend() == mount->getBackend()) {
        // The child is not a valid mountpoint.
        return -EINVAL;
      }
    }

    if (cwdIsAtOrBelow(cwdTransition, mount)) {
      return -EBUSY;
    }

    auto lockedParent = parent->locked();
    auto lookup = lockedParent.getChildWithError(childName);
    if (int err = lookup.getError()) {
      return err;
    }
    if (lookup.getFile() != mount) {
      continue;
    }
    // Input is valid, perform the unlink.
    return lockedParent.removeChild(childName);
  }
}

int __syscall_getdents64(int fd, intptr_t dirp, size_t count) {
  WASMFS_GUARD_NEGATIVE();
  dirent* result = (dirent*)dirp;

  // Check if the result buffer is too small.
  if (count / sizeof(dirent) == 0) {
    return -EINVAL;
  }

  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  auto lockedOpenFile = openFile->locked();

  auto dir = lockedOpenFile.getFile()->dynCast<Directory>();
  if (!dir) {
    return -ENOTDIR;
  }
  auto lockedDir = dir->locked();

  // If this directory has been unlinked and has no parent, then it is
  // completely empty.
  auto parent = lockedDir.getParent();
  if (!parent) {
    return 0;
  }

  const auto& dirents = openFile->dirents;
  // A directory's position corresponds to the index in its entries vector.
  // Keep it as an off_t so that an out-of-range directory cookie cannot wrap
  // to a negative index and poison the open file state.
  off_t index = lockedOpenFile.getPosition();
  if (index < 0) {
    return -EINVAL;
  }
  if (static_cast<__wasi_filesize_t>(index) >= dirents.size()) {
    return 0;
  }

  // Check every entry that can fit before modifying the output buffer or the
  // directory position. Backends may expose entries that were not created
  // through WasmFS's path validation.
  size_t entriesThatFit = count / sizeof(dirent);
  for (size_t candidate = static_cast<size_t>(index);
       candidate < dirents.size() && entriesThatFit;
       ++candidate, --entriesThatFit) {
    if (dirents[candidate].name.size() >= sizeof(result->d_name)) {
      return -ENAMETOOLONG;
    }
  }

  off_t bytesRead = 0;
  for (; static_cast<size_t>(index) < dirents.size() &&
         bytesRead + sizeof(dirent) <= count;
       index++) {
    const auto& entry = dirents[static_cast<size_t>(index)];
    result->d_ino = entry.ino;
    result->d_off = index + 1;
    result->d_reclen = sizeof(dirent);
    switch (entry.kind) {
      case File::UnknownKind:
        result->d_type = DT_UNKNOWN;
        break;
      case File::DataFileKind:
        result->d_type = DT_REG;
        break;
      case File::DirectoryKind:
        result->d_type = DT_DIR;
        break;
      case File::SymlinkKind:
        result->d_type = DT_LNK;
        break;
      default:
        result->d_type = DT_UNKNOWN;
        break;
    }
    strcpy(result->d_name, entry.name.c_str());
    ++result;
    bytesRead += sizeof(dirent);
  }

  // Update position
  lockedOpenFile.setPosition(index);

  return bytesRead;
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_renameat(int olddirfd,
                       intptr_t oldpath,
                       int newdirfd,
                       intptr_t newpath) {
  WASMFS_GUARD_NEGATIVE();
  // Hold this before renameMutex and path parsing. A rename can move the CWD
  // into a mount that another thread is checking for unmount.
  auto cwdTransition = wasmFS.beginCWDTransition();

  // Rename is the only syscall that needs to (or is allowed to) acquire locks
  // on two directories at once. It requires locks on both the old and new
  // parent directories to ensure that the moved file can be atomically removed
  // from the old directory and added to the new directory without something
  // changing that would prevent the move.
  //
  // To prevent deadlock in the case of simultaneous renames, serialize renames
  // with an additional global lock.
  static std::mutex renameMutex;
  std::lock_guard<std::mutex> renameLock(renameMutex);

  // Get the old directory.
  auto parsedOld = path::parseParent((char*)oldpath, olddirfd);
  if (auto err = parsedOld.getError()) {
    return err;
  }
  auto& [oldParent, oldFileNameView] = parsedOld.getParentChild();
  std::string oldFileName(oldFileNameView);

  // Get the new directory.
  auto parsedNew = path::parseParent((char*)newpath, newdirfd);
  if (auto err = parsedNew.getError()) {
    return err;
  }
  auto& [newParent, newFileNameView] = parsedNew.getParentChild();
  std::string newFileName(newFileNameView);

  if (newFileNameView.size() > WASMFS_NAME_MAX) {
    return -ENAMETOOLONG;
  }

  // Lock both directories.
  auto lockedOldParent = oldParent->locked();
  auto lockedNewParent = newParent->locked();

  // Get the source and destination files. Preserve source lookup failure
  // precedence so an I/O error never becomes a missing source or destination.
  auto oldLookup = lockedOldParent.getChildWithError(oldFileName);
  if (int err = oldLookup.getError()) {
    return err;
  }
  auto newLookup = lockedNewParent.getChildWithError(newFileName);
  if (int err = newLookup.getError()) {
    return err;
  }
  auto oldFile = oldLookup.getFile();
  auto newFile = newLookup.getFile();

  // Check either discovered leaf before returning a non-profile lookup
  // result. In particular, rename("missing", "/profile") must not reveal a
  // sealed mount as an ordinary ENOENT source failure.
  if (oldFile) {
    if (int err = admitFile(oldFile)) {
      return err;
    }
  }
  if (newFile) {
    if (int err = admitFile(newFile)) {
      return err;
    }
  }
  if (!oldFile) {
    return -ENOENT;
  }

  // If the source and destination are the same, do nothing.
  if (oldFile == newFile) {
    return 0;
  }

  // Never allow renaming or overwriting the root.
  auto root = wasmFS.getRootDirectory();
  if (oldFile == root || newFile == root) {
    return -EBUSY;
  }

  // Cannot modify either directory without write permissions.
  if (!(lockedOldParent.getMode() & WASMFS_PERM_WRITE) ||
      !(lockedNewParent.getMode() & WASMFS_PERM_WRITE)) {
    return -EACCES;
  }

  // Both parents must have the same backend.
  if (oldParent->getBackend() != newParent->getBackend()) {
    return -EXDEV;
  }

  // Check that oldDir is not an ancestor of newDir.
  for (auto curr = newParent; curr != root; curr = curr->locked().getParent()) {
    if (curr == oldFile) {
      return -EINVAL;
    }
  }

  // The new file will be removed if it already exists.
  if (newFile) {
    if (auto newDir = newFile->dynCast<Directory>()) {
      // Cannot overwrite a directory with a non-directory.
      auto oldDir = oldFile->dynCast<Directory>();
      if (!oldDir) {
        return -EISDIR;
      }
      // Cannot overwrite a non-empty directory.
      auto numEntries = newDir->locked().getNumEntries();
      if (numEntries < 0) {
        return numEntries;
      }
      if (numEntries > 0) {
        return -ENOTEMPTY;
      }
    } else {
      // Cannot overwrite a non-directory with a directory.
      if (oldFile->is<Directory>()) {
        return -ENOTDIR;
      }
    }
  }

  // Perform the move.
  if (auto err = lockedNewParent.insertMove(newFileName, oldFile)) {
    assert(err < 0);
    return err;
  }
  return 0;
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_symlinkat(intptr_t target, int newdirfd, intptr_t linkpath) {
  WASMFS_GUARD_NEGATIVE();
  auto parsed = path::parseParent((char*)linkpath, newdirfd);
  if (auto err = parsed.getError()) {
    return err;
  }
  auto& [parent, childNameView] = parsed.getParentChild();
  if (childNameView.size() > WASMFS_NAME_MAX) {
    return -ENAMETOOLONG;
  }
  auto lockedParent = parent->locked();
  std::string childName(childNameView);
  auto lookup = lockedParent.getChildWithError(childName);
  if (int err = lookup.getError()) {
    return err;
  }
  if (auto child = lookup.getFile()) {
    if (int err = admitFile(child)) {
      return err;
    }
    return -EEXIST;
  }
  if (!lockedParent.insertSymlink(childName, (char*)target)) {
    return -EPERM;
  }
  return 0;
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_readlinkat(int dirfd,
                         intptr_t path,
                         intptr_t buf,
                         size_t bufsize) {
  WASMFS_GUARD_NEGATIVE();
  // TODO: Handle empty paths.
  auto parsed = path::parseFile((char*)path, dirfd, path::NoFollowLinks);
  if (auto err = parsed.getError()) {
    return err;
  }
  auto link = parsed.getFile()->dynCast<Symlink>();
  if (!link) {
    return -EINVAL;
  }
  const auto& target = link->getTarget();
  auto bytes = std::min((size_t)bufsize, target.size());
  memcpy((char*)buf, target.c_str(), bytes);
  return bytes;
}

static double timespec_to_ms(timespec ts) {
  if (ts.tv_nsec == UTIME_OMIT) {
    return INFINITY;
  }
  if (ts.tv_nsec == UTIME_NOW) {
    return emscripten_date_now();
  }
  return double(ts.tv_sec) * 1000 + double(ts.tv_nsec) / (1000 * 1000);
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_utimensat(int dirFD, intptr_t path_, intptr_t times_, int flags) {
  WASMFS_GUARD_NEGATIVE();
  const char* path = (const char*)path_;
  const struct timespec* times = (const struct timespec*)times_;
  if (flags & ~AT_SYMLINK_NOFOLLOW) {
    // TODO: Test this case.
    return -EINVAL;
  }

  // Add AT_EMPTY_PATH as Linux (and so, musl, and us) has a nonstandard
  // behavior in which an empty path means to operate on whatever is in dirFD
  // (directory or not), which is exactly the behavior of AT_EMPTY_PATH (but
  // without passing that in). See "C library/kernel ABI differences" in
  // https://man7.org/linux/man-pages/man2/utimensat.2.html
  //
  // TODO: Handle AT_SYMLINK_NOFOLLOW once we traverse symlinks correctly.
  auto parsed =
    path::getFileAt(dirFD, path ? path : "", flags | AT_EMPTY_PATH);
  if (auto err = parsed.getError()) {
    return err;
  }

  // TODO: Handle tv_nsec being UTIME_NOW or UTIME_OMIT.
  // TODO: Check for write access to the file (see man page for specifics).
  double aTime, mTime;

  if (times == nullptr) {
    aTime = mTime = emscripten_date_now();
  } else {
    aTime = timespec_to_ms(times[0]);
    mTime = timespec_to_ms(times[1]);
  }

  // A pair of UTIME_OMIT values does not mutate any metadata, so it remains a
  // successful no-op even for a backend that cannot represent explicit
  // metadata changes.
  if (aTime == INFINITY && mTime == INFINITY) {
    return 0;
  }

  auto file = parsed.getFile();
  if (!canMutateExplicitMetadata(file)) {
    return -ENOTSUP;
  }

  auto locked = file->locked();
  auto metadata = locked.getMetadata();
  if (aTime != INFINITY) {
    metadata.atime = aTime;
  }
  if (mTime != INFINITY) {
    metadata.mtime = mTime;
  }
  return locked.setMetadata(metadata);
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_fchmodat2(int dirfd, intptr_t path, int mode, int flags) {
  WASMFS_GUARD_NEGATIVE();
  if (flags & ~AT_SYMLINK_NOFOLLOW) {
    // TODO: Test this case.
    return -EINVAL;
  }
  auto parsed = path::getFileAt(dirfd, (char*)path, flags);
  if (auto err = parsed.getError()) {
    return err;
  }
  auto file = parsed.getFile();
  if (!canMutateExplicitMetadata(file)) {
    return -ENOTSUP;
  }
  auto lockedFile = file->locked();
  auto metadata = lockedFile.getMetadata();
  metadata.mode = mode;
  // On POSIX, ctime is updated on metadata changes, like chmod.
  metadata.ctime = emscripten_date_now();
  return lockedFile.setMetadata(metadata);
}

int __syscall_chmod(intptr_t path, int mode) {
  WASMFS_GUARD_NEGATIVE();
  return __syscall_fchmodat2(AT_FDCWD, path, mode, 0);
}

int __syscall_fchmod(int fd, int mode) {
  WASMFS_GUARD_NEGATIVE();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  auto file = openFile->locked().getFile();
  if (!canMutateExplicitMetadata(file)) {
    return -ENOTSUP;
  }
  auto lockedFile = file->locked();
  auto metadata = lockedFile.getMetadata();
  metadata.mode = mode;
  metadata.ctime = emscripten_date_now();
  return lockedFile.setMetadata(metadata);
}

int __syscall_fchownat(
  int dirfd, intptr_t path, int owner, int group, int flags) {
  WASMFS_GUARD_NEGATIVE();
  // Only accept valid flags.
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
    // TODO: Test this case.
    return -EINVAL;
  }
  auto parsed = path::getFileAt(dirfd, (char*)path, flags);
  if (auto err = parsed.getError()) {
    return err;
  }

  // WasmFS does not track ownership metadata, so an ownership change cannot
  // succeed. Resolve the target first so validation and lookup errors retain
  // their normal precedence.
  return owner == -1 && group == -1 ? 0 : -ENOTSUP;
}

int __syscall_fchown32(int fd, int owner, int group) {
  WASMFS_GUARD_NEGATIVE();
  return __syscall_fchownat(fd, (intptr_t) "", owner, group, AT_EMPTY_PATH);
}

// TODO: Test this with non-AT_FDCWD values.
int __syscall_faccessat(int dirfd, intptr_t path, int amode, int flags) {
  WASMFS_GUARD_NEGATIVE();
  // The input must be F_OK (check for existence) or a combination of [RWX]_OK
  // flags.
  if (amode != F_OK && (amode & ~(R_OK | W_OK | X_OK))) {
    return -EINVAL;
  }
  if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) {
    // TODO: Test this case.
    return -EINVAL;
  }

  // TODO: Handle AT_SYMLINK_NOFOLLOW once we traverse symlinks correctly.
  auto parsed = path::parseFile((char*)path, dirfd);
  if (auto err = parsed.getError()) {
    return err;
  }

  if (amode != F_OK) {
    auto mode = parsed.getFile()->locked().getMode();
    if ((amode & R_OK) && !(mode & WASMFS_PERM_READ)) {
      return -EACCES;
    }
    if ((amode & W_OK) && !(mode & WASMFS_PERM_WRITE)) {
      return -EACCES;
    }
    if ((amode & X_OK) && !(mode & WASMFS_PERM_EXECUTE)) {
      return -EACCES;
    }
  }

  return 0;
}

// Pathname truncation uses the file's current mode to authorize the operation.
// ftruncate has already authorized a writable open file description instead;
// that authorization remains valid if the logical file mode changes later.
static int doTruncate(std::shared_ptr<File>& file,
                      off_t size,
                      bool checkFileWritePermission) {
  auto dataFile = file->dynCast<DataFile>();

  if (!dataFile) {
    return -EISDIR;
  }

  auto locked = dataFile->locked();
  if (checkFileWritePermission &&
      !(locked.getMode() & WASMFS_PERM_WRITE)) {
    return -EACCES;
  }

  if (size < 0) {
    return -EINVAL;
  }

  int ret = resizeDataFile(dataFile, locked, size);
  assert(ret <= 0);
  return ret;
}

int __syscall_truncate64(intptr_t path, off_t size) {
  WASMFS_GUARD_NEGATIVE();
  auto parsed = path::parseFile((char*)path);
  if (auto err = parsed.getError()) {
    return err;
  }
  return doTruncate(parsed.getFile(), size, true);
}

int __syscall_ftruncate64(int fd, off_t size) {
  WASMFS_GUARD_NEGATIVE();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  auto lockedOpenFile = openFile->locked();
  if ((lockedOpenFile.getFlags() & O_ACCMODE) == O_RDONLY) {
    return -EINVAL;
  }
  return doTruncate(lockedOpenFile.getFile(), size, false);
}

static bool isTTY(std::shared_ptr<File>& file) {
  // TODO: Full TTY support. For now, just see stdin/out/err as terminals and
  //       nothing else.
  return file == SpecialFiles::getStdin() ||
         file == SpecialFiles::getStdout() || file == SpecialFiles::getStderr();
}

int __syscall_ioctl(int fd, int request, ...) {
  WASMFS_GUARD_NEGATIVE();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  if (!isTTY(openFile->locked().getFile())) {
    return -ENOTTY;
  }
  // TODO: Full TTY support. For now this is limited, and matches the old FS.
  switch (request) {
    case TCGETA:
    case TCGETS:
    case TCSETA:
    case TCSETAW:
    case TCSETAF:
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TIOCGWINSZ:
    case TIOCSWINSZ: {
      // TTY operations that we do nothing for anyhow can just be ignored.
      return 0;
    }
    default: {
      return -EINVAL; // not supported
    }
  }
}

int __syscall_pipe2(intptr_t fd, int flags) {
  WASMFS_GUARD_NEGATIVE();
  auto* fds = (__wasi_fd_t*)fd;
  if (flags && flags != O_CLOEXEC) {
    return -ENOTSUP;
  }

  // Make a pipe: Two PipeFiles that share a single data source between them, so
  // that writing to one can be read in the other.
  //
  // No backend is needed here, so pass in nullptr for that.
  auto data = std::make_shared<PipeData>();
  auto reader = std::make_shared<PipeFile>(S_IRUGO, data);
  auto writer = std::make_shared<PipeFile>(S_IWUGO, data);

  std::shared_ptr<OpenFileState> openReader, openWriter;
  (void)OpenFileState::create(reader, O_RDONLY, openReader);
  (void)OpenFileState::create(writer, O_WRONLY, openWriter);

  std::shared_ptr<DataFile> closee;
  int readerFD = -EMFILE;
  int writerFD = -EMFILE;
  {
    auto fileTable = wasmFS.getFileTable().locked();
    readerFD = fileTable.addEntry(openReader);
    if (readerFD >= 0) {
      writerFD = fileTable.addEntry(openWriter);
      if (writerFD >= 0) {
        // pipe2() must leave pipefd unchanged on failure, so do not publish
        // either descriptor until both ends have been installed.
        fds[0] = readerFD;
        fds[1] = writerFD;
        return 0;
      }

      // The reader was installed but the writer could not be. Remove the
      // reader before releasing the lock, then close it below.
      closee = fileTable.setEntry(readerFD, nullptr);
    }
  }

  if (closee) {
    (void)closee->locked().close();
  }
  if (readerFD < 0) {
    (void)abandonOpenFile(std::move(openReader));
  }
  (void)abandonOpenFile(std::move(openWriter));

  return readerFD < 0 ? readerFD : writerFD;
}

// int poll(struct pollfd* fds, nfds_t nfds, int timeout);
int __syscall_poll(intptr_t fds_, int nfds, int timeout) {
  WASMFS_GUARD_NEGATIVE();
  struct pollfd* fds = (struct pollfd*)fds_;
  auto fileTable = wasmFS.getFileTable().locked();

  // Process the list of FDs and compute their revents masks. Count the number
  // of nonzero such masks, which is our return value.
  int nonzero = 0;
  for (nfds_t i = 0; i < nfds; i++) {
    auto* pollfd = &fds[i];
    auto fd = pollfd->fd;
    if (fd < 0) {
      // Negative FDs are ignored in poll().
      pollfd->revents = 0;
      continue;
    }
    // Assume invalid, unless there is an open file.
    auto mask = POLLNVAL;
    auto openFile = fileTable.getEntry(fd);
    if (openFile) {
      if (int err = admitOpenFile(openFile)) {
        return err;
      }
      mask = 0;
      auto flags = openFile->locked().getFlags();
      auto accessMode = flags & O_ACCMODE;
      auto readBit = pollfd->events & POLLOUT;
      if (readBit && (accessMode == O_WRONLY || accessMode == O_RDWR)) {
        mask |= readBit;
      }
      auto writeBit = pollfd->events & POLLIN;
      if (writeBit && (accessMode == O_RDONLY || accessMode == O_RDWR)) {
        // If there is data in the file, then there is also the ability to read.
        // TODO: Does this need to consider the position as well? That is, if
        // the position is at the end, we can't read from the current position
        // at least. If we update this, make sure the size isn't an error!
        if (openFile->locked().getFile()->locked().getSize() > 0) {
          mask |= writeBit;
        }
      }
      // TODO: get mask from File dynamically using a poll() hook?
    }
    // TODO: set the state based on the state of the other end of the pipe, for
    //       pipes (POLLERR | POLLHUP)
    if (mask) {
      nonzero++;
    }
    pollfd->revents = mask;
  }
  // TODO: This should block based on the timeout. The old FS did not do so due
  //       to web limitations, which we should perhaps revisit (especially with
  //       pthreads and asyncify).
  return nonzero;
}

int __syscall_fallocate(int fd, int mode, off_t offset, off_t len) {
  WASMFS_GUARD_NEGATIVE();
  auto fileTable = wasmFS.getFileTable().locked();
  auto openFile = fileTable.getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }

  // Match Linux's validation order: after resolving the descriptor, reject an
  // invalid range before evaluating the requested allocation mode.
  if (offset < 0 || len <= 0) {
    return -EINVAL;
  }

  // WasmFS can grow a file, but has no backend API for reserving space while
  // preserving its size, punching holes, or any other fallocate mode. Do not
  // treat those requests as the default allocation operation in release
  // builds, where an assert would otherwise disappear.
  if (mode != 0) {
    return -ENOTSUP;
  }

  auto lockedOpenFile = openFile->locked();
  if ((lockedOpenFile.getFlags() & O_ACCMODE) == O_RDONLY) {
    return -EBADF;
  }

  auto file = lockedOpenFile.getFile();
  // A pipe can have buffered data already covering the requested range, in
  // which case a size comparison alone would report a false success without
  // calling PipeFile::setSize().
  if (!file->isSeekable()) {
    return -ESPIPE;
  }

  auto dataFile = file->dynCast<DataFile>();
  // TODO: support for symlinks.
  if (!dataFile) {
    return -ENODEV;
  }

  // The writable open file description above authorizes this operation even
  // if a later chmod changes the logical file mode.
  auto locked = dataFile->locked();

  // `offset + len` must not wrap and turn a request too large for WasmFS into
  // a successful no-op.
  if (offset > std::numeric_limits<off_t>::max() - len) {
    return -EFBIG;
  }

  // TODO: We could only fill zeros for regions that were completely unused
  //       before, which for a backend with sparse data storage could make a
  //       difference. For that we'd need a new backend API.
  auto newNeededSize = offset + len;
  off_t size = locked.getSize();
  if (size < 0) {
    return size;
  }
  if (newNeededSize > size) {
    if (auto err = resizeDataFile(dataFile, locked, newNeededSize)) {
      assert(err < 0);
      return err;
    }
  }

  return 0;
}

int __syscall_fcntl64(int fd, int cmd, ...) {
  WASMFS_GUARD_NEGATIVE();
  if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
    std::shared_ptr<OpenFileState> openFile;
    {
      // Record-lock normalization can query an OPFS file's size. Do not hold
      // the descriptor-table mutex over that proxying operation.
      auto fileTable = wasmFS.getFileTable().locked();
      openFile = fileTable.getEntry(fd);
    }
    if (!openFile) {
      return -EBADF;
    }
    if (int err = admitOpenFile(openFile)) {
      return err;
    }

    struct flock* lock;
    va_list v1;
    va_start(v1, cmd);
    lock = va_arg(v1, struct flock*);
    va_end(v1);

    if (cmd == F_GETLK) {
      // If these constants differ then we'd need a case for both.
      static_assert(F_GETLK == F_GETLK64);
      return handleRecordLock(openFile, lock, true);
    }

    static_assert(F_SETLK == F_SETLK64);
    static_assert(F_SETLKW == F_SETLKW64);
    return handleRecordLock(openFile, lock, false);
  }

  auto fileTable = wasmFS.getFileTable().locked();
  auto openFile = fileTable.getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }

  switch (cmd) {
    case F_DUPFD: {
      int newfd;
      va_list v1;
      va_start(v1, cmd);
      newfd = va_arg(v1, int);
      va_end(v1);
      if (newfd < 0 || newfd >= WASMFS_FD_MAX) {
        return -EINVAL;
      }

      // Find the first available fd at arg or after.
      for (; newfd < WASMFS_FD_MAX; ++newfd) {
        if (!fileTable.getEntry(newfd)) {
          (void)fileTable.setEntry(newfd, openFile);
          return newfd;
        }
      }
      return -EMFILE;
    }
    case F_GETFD:
    case F_SETFD:
      // FD_CLOEXEC makes no sense for a single process.
      return 0;
    case F_GETFL:
      return openFile->locked().getFlags();
    case F_SETFL: {
      int flags;
      va_list v1;
      va_start(v1, cmd);
      flags = va_arg(v1, int);
      va_end(v1);
      auto lockedOpenFile = openFile->locked();
      auto oldFlags = lockedOpenFile.getFlags();
      // This syscall should ignore most flags.
      flags = flags & ~(O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_EXCL |
                        O_NOCTTY | O_TRUNC);
      // Also ignore this flag which musl always adds constantly, but does not
      // matter for us.
      flags = flags & ~O_LARGEFILE;
      // On linux only a few flags can be modified, and we support only a subset
      // of those. Error on anything else.
      auto supportedFlags = flags & O_APPEND;
      if (flags != supportedFlags) {
        return -EINVAL;
      }
      // F_SETFL can change O_APPEND, but must preserve the descriptor's
      // immutable flags, including its access mode.
      lockedOpenFile.setFlags((oldFlags & ~O_APPEND) | supportedFlags);
      return 0;
    }
    default: {
      // TODO: support any remaining cmds
      return -EINVAL;
    }
  }
}

#ifdef WASMFS_RECORD_LOCK_TEST
// This symbol is deliberately absent from production builds and public
// headers. POSIX F_GETLK hides locks owned by the calling process, so the
// focused WasmFS test uses this internal counter to verify any-close release
// semantics for duplicated descriptors.
extern "C" int wasmfs_record_lock_count_for_testing(int fd) {
  WASMFS_GUARD_NEGATIVE();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  auto file = openFile->locked().getFile();
  return static_cast<int>(file->locked().getRecordLockCount());
}
#endif

static int
doStatFS(std::shared_ptr<File>& file, size_t size, struct statfs* buf) {
  if (size != sizeof(struct statfs)) {
    // We only know how to write to a standard statfs, not even a truncated one.
    return -EINVAL;
  }

  // NOTE: None of the constants here are true. We're just returning safe and
  //       sane values, that match the long-existing JS FS behavior (except for
  //       the inode number, where we can do better).
  buf->f_type = 0;
  buf->f_bsize = 4096;
  buf->f_frsize = 4096;
  buf->f_blocks = 1000000;
  buf->f_bfree = 500000;
  buf->f_bavail = 500000;
  buf->f_files = file->getIno();
  buf->f_ffree = 1000000;
  buf->f_fsid = {0, 0};
  buf->f_flags = ST_NOSUID;
  buf->f_namelen = 255;
  return 0;
}

int __syscall_statfs64(intptr_t path, size_t size, intptr_t buf) {
  WASMFS_GUARD_NEGATIVE();
  auto parsed = path::parseFile((char*)path);
  if (auto err = parsed.getError()) {
    return err;
  }
  return doStatFS(parsed.getFile(), size, (struct statfs*)buf);
}

int __syscall_fstatfs64(int fd, size_t size, intptr_t buf) {
  WASMFS_GUARD_NEGATIVE();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  return doStatFS(openFile->locked().getFile(), size, (struct statfs*)buf);
}

int _mmap_js(size_t length,
             int prot,
             int flags,
             int fd,
             off_t offset,
             int* allocated,
             void** addr) {
  WASMFS_GUARD_NEGATIVE();
  // PROT_EXEC is not supported (although we pretend to support the absence of
  // PROT_READ or PROT_WRITE).
  if ((prot & PROT_EXEC)) {
    return -EPERM;
  }

  if (!length) {
    return -EINVAL;
  }

  // One of MAP_PRIVATE, MAP_SHARED, or MAP_SHARED_VALIDATE must be used.
  int mapType = flags & MAP_TYPE;
  if (mapType != MAP_PRIVATE && mapType != MAP_SHARED &&
      mapType != MAP_SHARED_VALIDATE) {
    return -EINVAL;
  }

  if (mapType == MAP_SHARED_VALIDATE) {
    WASMFS_UNREACHABLE("TODO: MAP_SHARED_VALIDATE");
  }

  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }

  std::shared_ptr<DataFile> file;

  // Keep the open file info locked only for as long as we need that.
  {
    auto lockedOpenFile = openFile->locked();

    // Check permissions. We always need read permissions, since we need to read
    // the data in the file to map it.
    if ((lockedOpenFile.getFlags() & O_ACCMODE) == O_WRONLY) {
      return -EACCES;
    }

    // According to the POSIX spec it is possible to write to a file opened in
    // read-only mode with MAP_PRIVATE flag, as all modifications will be
    // visible only in the memory of the current process.
    if ((prot & PROT_WRITE) != 0 && mapType != MAP_PRIVATE &&
        (lockedOpenFile.getFlags() & O_ACCMODE) != O_RDWR) {
      return -EACCES;
    }

    file = lockedOpenFile.getFile()->dynCast<DataFile>();
  }

  if (!file) {
    return -ENODEV;
  }

  // WasmFS currently maps a copy and writes it back through an fd on sync.
  // It cannot provide MAP_SHARED writable mapping lifetime, coherence, or
  // writeback error semantics, so fail instead of accepting unsafe writes.
  if (!(flags & MAP_ANONYMOUS) && mapType == MAP_SHARED &&
      (prot & PROT_WRITE)) {
    return -ENOTSUP;
  }

  // TODO: On MAP_SHARED, install the mapping on the DataFile object itself so
  // that reads and writes can be redirected to the mapped region and so that
  // the mapping can correctly outlive the file being closed. This will require
  // changes to emscripten_mmap.c as well.

  // Align to a wasm page size, as we expect in the future to get wasm
  // primitives to do this work, and those would presumably be aligned to a page
  // size. Aligning now avoids confusion later.
  uint8_t* ptr = (uint8_t*)emscripten_builtin_memalign(WASM_PAGE_SIZE, length);
  if (!ptr) {
    return -ENOMEM;
  }

  auto nread = file->locked().read(ptr, length, offset);
  if (nread < 0) {
    // The read failed. Report the error, but first free the allocation.
    emscripten_builtin_free(ptr);
    return nread;
  }

  // From here on, we have succeeded, and can mark the allocation as having
  // occurred (which means that the caller has the responsibility to free it).
  *allocated = true;
  *addr = (void*)ptr;

  // The read must be of a valid amount, or we have had an internal logic error.
  assert(nread <= length);

  // mmap clears any extra bytes after the data itself.
  memset(ptr + nread, 0, length - nread);

  return 0;
}

int _msync_js(
  intptr_t addr, size_t length, int prot, int flags, int fd, off_t offset) {
  WASMFS_GUARD_NEGATIVE();
  // TODO: This is not correct! Mappings should be associated with files, not
  // fds. Only need to sync if shared and writes are allowed.
  int mapType = flags & MAP_TYPE;
  if (mapType == MAP_SHARED && (prot & PROT_WRITE)) {
    __wasi_ciovec_t iovec;
    iovec.buf = (uint8_t*)addr;
    iovec.buf_len = length;
    __wasi_size_t nwritten;
    // Translate from WASI positive error codes to negative error codes.
    return -__wasi_fd_pwrite(fd, &iovec, 1, offset, &nwritten);
  }
  return 0;
}

int _munmap_js(
  intptr_t addr, size_t length, int prot, int flags, int fd, off_t offset) {
  WASMFS_GUARD_NEGATIVE();
  // TODO: This is not correct! Mappings should be associated with files, not
  // fds.
  // TODO: Syncing should probably be handled in __syscall_munmap instead.
  return _msync_js(addr, length, prot, flags, fd, offset);
}

// Stubs (at least for now)

int __syscall_accept4(int sockfd,
                      intptr_t addr,
                      intptr_t addrlen,
                      int flags,
                      int dummy1,
                      int dummy2) {
  return -ENOSYS;
}

int __syscall_bind(
  int sockfd, intptr_t addr, size_t alen, int dummy, int dummy2, int dummy3) {
  return -ENOSYS;
}

int __syscall_connect(
  int sockfd, intptr_t addr, size_t len, int dummy, int dummy2, int dummy3) {
  return -ENOSYS;
}

int __syscall_socket(
  int domain, int type, int protocol, int dummy1, int dummy2, int dummy3) {
  return -ENOSYS;
}

int __syscall_listen(
  int sockfd, int backlog, int dummy1, int dummy2, int dummy3, int dummy4) {
  return -ENOSYS;
}

int __syscall_getsockopt(int sockfd,
                         int level,
                         int optname,
                         intptr_t optval,
                         intptr_t optlen,
                         int dummy) {
  return -ENOSYS;
}

int __syscall_getsockname(
  int sockfd, intptr_t addr, intptr_t len, int dummy, int dummy2, int dummy3) {
  return -ENOSYS;
}

int __syscall_getpeername(
  int sockfd, intptr_t addr, intptr_t len, int dummy, int dummy2, int dummy3) {
  return -ENOSYS;
}

int __syscall_sendto(
  int sockfd, intptr_t msg, size_t len, int flags, intptr_t addr, size_t alen) {
  return -ENOSYS;
}

int __syscall_sendmsg(
  int sockfd, intptr_t msg, int flags, intptr_t addr, size_t alen, int dummy) {
  return -ENOSYS;
}

int __syscall_recvfrom(int sockfd,
                       intptr_t msg,
                       size_t len,
                       int flags,
                       intptr_t addr,
                       intptr_t alen) {
  return -ENOSYS;
}

int __syscall_recvmsg(
  int sockfd, intptr_t msg, int flags, int dummy, int dummy2, int dummy3) {
  return -ENOSYS;
}

int __syscall_fadvise64(int fd, off_t offset, off_t length, int advice) {
  WASMFS_GUARD_NEGATIVE();
  auto openFile = wasmFS.getFileTable().locked().getEntry(fd);
  if (!openFile) {
    return -EBADF;
  }
  if (int err = admitOpenFile(openFile)) {
    return err;
  }
  // Advice is currently ignored. TODO some backends might use it
  return 0;
}

#undef WASMFS_GUARD_BACKEND
#undef WASMFS_GUARD_WASI_STDIO_WRITE
#undef WASMFS_GUARD_WASI
#undef WASMFS_GUARD_NEGATIVE

} // extern "C"
