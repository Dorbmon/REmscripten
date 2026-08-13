/*
 * Copyright 2022 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <emscripten/heap.h>

#include "emscripten_internal.h"
#include "lock.h"
#include "syscall.h"

// These private optional references keep the mmap registry independent of a
// WasmFS implementation.  Full WasmFS provides strong definitions so its
// terminal admission spans the registry update below.  A build without that
// implementation leaves the references null and retains its existing mmap
// behavior.  They make no claim about -sWASMFS source selection.
struct wasmfs_operation_handle;
struct wasmfs_operation_handle* __wasmfs_acquire_operation(void)
  __attribute__((__weak__));
void __wasmfs_release_operation(struct wasmfs_operation_handle* handle)
  __attribute__((__weak__));

struct map {
  void* addr;
  long length;
  int allocated;
  int fd;
  int flags;
  off_t offset;
  int prot;
  struct map* next;
} __attribute__((aligned (1)));

#define ALIGN_TO(value,alignment) (((value) + ((alignment) - 1)) & ~((alignment) - 1))

// Linked list of all mapping, guarded by a musl-style lock (LOCK/UNLOCK)
static volatile int lock[1];
static struct map* mappings;

static struct map* find_mapping(intptr_t addr, struct map** prev) {
  struct map* map = mappings;
  while (map) {
    if (map->addr == (void*)addr) {
      return map;
    }
    if (prev) {
      *prev = map;
    }
    map = map->next;
  }
  return map;
}

// The WasmFS implementation of _mmap_js/_msync_js/_munmap_js has its own
// operation guards, but this outer layer mutates the mapping registry after
// those calls return.  A full-WasmFS build holds the same admission across the
// whole file-backed syscall so a terminal drain cannot detach descriptors
// between the JS operation and that registry mutation.  Anonymous mappings do
// not touch WasmFS and intentionally remain usable after a terminal drain.
static struct wasmfs_operation_handle* acquire_wasmfs_operation(void) {
  if (!__wasmfs_acquire_operation) {
    return NULL;
  }
  return __wasmfs_acquire_operation();
}

static void
release_wasmfs_operation(struct wasmfs_operation_handle* operation) {
  if (operation && __wasmfs_release_operation) {
    __wasmfs_release_operation(operation);
  }
}

int __syscall_munmap(intptr_t addr, size_t length) {
  struct wasmfs_operation_handle* operation = NULL;
  int result = 0;
  LOCK(lock);
  struct map* prev = NULL;
  struct map* map = find_mapping(addr, &prev);
  if (!map || !length) {
    UNLOCK(lock);
    result = -EINVAL;
    goto done;
  }

  // We don't support partial munmapping.
  if (map->length != length) {
    UNLOCK(lock);
    result = -EINVAL;
    goto done;
  }

  if (!(map->flags & MAP_ANONYMOUS)) {
    // Do not hold the mapping-list lock while acquiring an Operation: a
    // blocking filesystem operation can itself lead to a mapping call. A
    // concurrent munmap can remove and reuse this address, so revalidate the
    // mapping after reacquiring the registry lock.
    UNLOCK(lock);
    operation = acquire_wasmfs_operation();
    if (__wasmfs_acquire_operation && !operation) {
      return -(errno ? errno : EIO);
    }
    LOCK(lock);
    prev = NULL;
    map = find_mapping(addr, &prev);
    if (!map || !length || map->length != length) {
      UNLOCK(lock);
      result = -EINVAL;
      goto done;
    }
    if (map->flags & MAP_ANONYMOUS) {
      // The address was reused while the lock was released. Anonymous
      // mappings are intentionally usable after terminal drain, so discard
      // the no-longer-needed filesystem admission before removing it.
      UNLOCK(lock);
      release_wasmfs_operation(operation);
      operation = NULL;
      LOCK(lock);
      prev = NULL;
      map = find_mapping(addr, &prev);
      if (!map || !length || map->length != length ||
          !(map->flags & MAP_ANONYMOUS)) {
        UNLOCK(lock);
        result = -EINVAL;
        goto done;
      }
    }
  }

  // Remove map from linked list
  if (prev) {
    prev->next = map->next;
  } else {
    mappings = map->next;
  }
  UNLOCK(lock);

  if (!(map->flags & MAP_ANONYMOUS)) {
    _munmap_js(addr, length, map->prot, map->flags, map->fd, map->offset);
  }

  // Release the memory.
  if (map->allocated) {
    emscripten_builtin_free(map->addr);
  }

  if (!(map->flags & MAP_ANONYMOUS)) {
    emscripten_builtin_free(map);
  }

  result = 0;

done:
  release_wasmfs_operation(operation);
  return result;
}

int __syscall_msync(intptr_t addr, size_t len, int flags) {
  struct wasmfs_operation_handle* operation = NULL;
  int result = 0;
  LOCK(lock);
  struct map* map = find_mapping(addr, NULL);
  if (!map) {
    UNLOCK(lock);
    result = -EINVAL;
    goto done;
  }
  if (map->flags & MAP_ANONYMOUS) {
    UNLOCK(lock);
    goto done;
  }
  // See munmap for why this acquisition happens outside the mapping-list
  // lock. msync does not mutate the registry; copy fields before dropping the
  // lock so a concurrent munmap cannot free its record while it is in use.
  int prot = map->prot;
  int map_flags = map->flags;
  int fd = map->fd;
  off_t offset = map->offset;
  UNLOCK(lock);
  operation = acquire_wasmfs_operation();
  if (__wasmfs_acquire_operation && !operation) {
    return -(errno ? errno : EIO);
  }
  result = _msync_js(addr, len, prot, map_flags, fd, offset);

done:
  release_wasmfs_operation(operation);
  return result;
}

intptr_t __syscall_mmap2(intptr_t addr, size_t len, int prot, int flags, int fd, off_t offset) {
  struct wasmfs_operation_handle* operation = NULL;
  if (!(flags & MAP_ANONYMOUS)) {
    operation = acquire_wasmfs_operation();
    if (__wasmfs_acquire_operation && !operation) {
      return -(errno ? errno : EIO);
    }
  }

  intptr_t result;
  if (addr != 0) {
    // We don't currently support location hints for the address of the mapping
    result = -EINVAL;
    goto done;
  }

  offset *= SYSCALL_MMAP2_UNIT;
  struct map* new_map;

  // MAP_ANONYMOUS (aka MAP_ANON) isn't actually defined by POSIX spec,
  // but it is widely used way to allocate memory pages on Linux, BSD and Mac.
  // In this case fd argument is ignored.
  if (flags & MAP_ANONYMOUS) {
    size_t alloc_len = ALIGN_TO(len, 16);
    // For anonymous maps, allocate that mapping at the end of the region.
    void* ptr = emscripten_builtin_memalign(WASM_PAGE_SIZE, alloc_len + sizeof(struct map));
    if (!ptr) {
      result = -ENOMEM;
      goto done;
    }
    memset(ptr, 0, alloc_len);
    new_map = (struct map*)((char*)ptr + alloc_len);
    new_map->addr = ptr;
    new_map->fd = -1;
    new_map->allocated = true;
  } else {
    new_map = emscripten_builtin_malloc(sizeof(struct map));
    int rtn =
      _mmap_js(len, prot, flags, fd, offset, &new_map->allocated, &new_map->addr);
    if (rtn < 0) {
      emscripten_builtin_free(new_map);
      result = rtn;
      goto done;
    }
    new_map->fd = fd;
  }

  new_map->length = len;
  new_map->flags = flags;
  new_map->offset = offset;
  new_map->prot = prot;

  LOCK(lock);
  new_map->next = mappings;
  mappings = new_map;
  UNLOCK(lock);

  result = (long)new_map->addr;

done:
  release_wasmfs_operation(operation);
  return result;
}
