#include <vector>

#include <emscripten/proxying.h>

#include "backend.h"

using namespace wasmfs;

extern "C" {

// Ensure that the root OPFS directory is initialized in permanent handle ID 1.
// HandleAllocator reserves ID 0.
void _wasmfs_opfs_init_root_directory(em_proxying_ctx* ctx);

// Acquire and release the opt-in storage-bucket-scoped OPFS profile lease.
// These run on the OPFS backend's dedicated worker and do not touch OPFS
// handles. The Web Locks lease is cooperative, not physical OPFS ownership.
void _wasmfs_opfs_acquire_profile_lease(em_proxying_ctx* ctx,
                                        const char* profile_name,
                                        int* err);

void _wasmfs_opfs_release_profile_lease(em_proxying_ctx* ctx, int* err);

// Check that descriptor teardown left no browser-owned access/blob state
// before a scoped profile handoff may release its Web Lock.
void _wasmfs_opfs_prepare_profile_retirement(em_proxying_ctx* ctx, int* err);

// One dedicated-worker transaction for the irrevocable scoped handoff. It
// writes whether Web Locks release was acknowledged separately from any later
// worker-context cleanup error, then clears OPFS allocators/lease state and
// stops the heartbeat before native cancellation may occur.
void _wasmfs_opfs_release_profile_lease_and_retire_context(
  em_proxying_ctx* ctx,
  em_proxying_queue* queue,
  int* lease_released,
  int* err);

// Look up the child under `parent` with `name`. Write 1 to `child_type` if it's
// a regular file or 2 if it's a directory. For a regular file, write zero to
// `child_id`: its OPFSFile wrapper acquires a FileSystemFileHandle lazily. For
// a directory, write its directory ID. Write a negative error to `child_id` if
// the child does not exist or cannot be opened.
void _wasmfs_opfs_get_child(em_proxying_ctx* ctx,
                            int parent,
                            const char* name,
                            int* child_type,
                            int* child_id);

// Create a file under `parent` with `name` and write zero to `child_id`. Its
// OPFSFile wrapper acquires a FileSystemFileHandle lazily.
void _wasmfs_opfs_insert_file(em_proxying_ctx* ctx,
                              int parent,
                              const char* name,
                              int* child_id);

// Acquire a FileSystemFileHandle for an existing regular file. This is used
// when an OPFSFile retains its WasmFS identity but has released its idle JS
// file-handle reference. Write the handle ID or a negative errno to
// `file_id`.
void _wasmfs_opfs_acquire_file(em_proxying_ctx* ctx,
                               int parent,
                               const char* name,
                               int* file_id);

// Create a directory under `parent` with `name` and store its ID in `child_id`.
void _wasmfs_opfs_insert_directory(em_proxying_ctx* ctx,
                                   int parent,
                                   const char* name,
                                   int* child_id);

void _wasmfs_opfs_move_file(em_proxying_ctx* ctx,
                            int file_id,
                            int new_parent_id,
                            const char* name,
                            int* err);

void _wasmfs_opfs_remove_child(em_proxying_ctx* ctx,
                               int dir_id,
                               const char* name,
                               int* err);

void _wasmfs_opfs_get_entries(em_proxying_ctx* ctx,
                              int dirID,
                              std::vector<Directory::Entry>* entries,
                              int* err);

void _wasmfs_opfs_open_access(em_proxying_ctx* ctx,
                              int file_id,
                              int* access_id);

void _wasmfs_opfs_open_blob(em_proxying_ctx* ctx, int file_id, int* blob_id);

void _wasmfs_opfs_close_access(em_proxying_ctx* ctx, int access_id, int* err);

void _wasmfs_opfs_close_blob(int blob_id);

void _wasmfs_opfs_free_file(int file_id);

void _wasmfs_opfs_free_directory(int dir_id);

// Synchronous read. Return the number of bytes read.
int _wasmfs_opfs_read_access(int access_id,
                             uint8_t* buf,
                             uint32_t len,
                             off_t pos);

int _wasmfs_opfs_read_blob(em_proxying_ctx* ctx,
                           int blob_id,
                           uint8_t* buf,
                           uint32_t len,
                           off_t pos,
                           int32_t* nread);

// Synchronous write. Return the number of bytes written.
int _wasmfs_opfs_write_access(int access_id,
                              const uint8_t* buf,
                              uint32_t len,
                              off_t pos);

// Get the size via an AccessHandle.
void _wasmfs_opfs_get_size_access(em_proxying_ctx* ctx,
                                  int access_id,
                                  off_t* size);

off_t _wasmfs_opfs_get_size_blob(int blob_id);

// Get the size of a file handle via a File Blob.
void _wasmfs_opfs_get_size_file(em_proxying_ctx* ctx, int file_id, off_t* size);

void _wasmfs_opfs_set_size_access(em_proxying_ctx* ctx,
                                  int access_id,
                                  off_t size,
                                  int* err);

void _wasmfs_opfs_set_size_file(em_proxying_ctx* ctx,
                                int file_id,
                                off_t size,
                                int* err);

void _wasmfs_opfs_flush_access(em_proxying_ctx* ctx, int access_id, int* err);

} // extern "C"
