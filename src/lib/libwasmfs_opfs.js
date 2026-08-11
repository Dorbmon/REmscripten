/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

#if WASMFS_OPFS_TEST_MOVE_INTERRUPT != 0 && WASMFS_OPFS_TEST_MOVE_INTERRUPT != 1 && WASMFS_OPFS_TEST_MOVE_INTERRUPT != 2
#error "WASMFS_OPFS_TEST_MOVE_INTERRUPT must be 0, 1, or 2"
#endif

#if WASMFS_OPFS_TEST_CLOSE_FAILURE != 0 && WASMFS_OPFS_TEST_CLOSE_FAILURE != 1
#error "WASMFS_OPFS_TEST_CLOSE_FAILURE must be 0 or 1"
#endif

#if WASMFS_OPFS_TEST_FILE_HANDLE_CACHE != 0 && WASMFS_OPFS_TEST_FILE_HANDLE_CACHE != 1
#error "WASMFS_OPFS_TEST_FILE_HANDLE_CACHE must be 0 or 1"
#endif

addToLibrary({
  $wasmfsOPFSDirectoryHandles__deps: ['$HandleAllocator'],
  $wasmfsOPFSDirectoryHandles: "new HandleAllocator()",
  $wasmfsOPFSFileHandles__deps: ['$HandleAllocator'],
  $wasmfsOPFSFileHandles: "new HandleAllocator()",
  $wasmfsOPFSAccessHandles__deps: ['$HandleAllocator'],
  $wasmfsOPFSAccessHandles: "new HandleAllocator()",
  $wasmfsOPFSBlobs__deps: ["$HandleAllocator"],
  $wasmfsOPFSBlobs: "new HandleAllocator()",
  // This state lives in an OPFS backend's dedicated worker. A lease is held
  // by keeping the Web Locks callback pending until C++ tears the backend down.
  $wasmfsOPFSProfileLease: {
    release: undefined,
    request: undefined,
  },

#if WASMFS_OPFS_TEST_MOVE_INTERRUPT
  // This is compiled only into the focused interruption test. It has no
  // production configuration surface: once the witness is emitted, leave the
  // proxy callback pending until its containing document is disposed.
  $wasmfsOPFSTestMoveInterrupt: async (phase) => {
    let channel = new BroadcastChannel('wasmfs-opfs-test-move-interrupt');
    channel.postMessage({
      phase,
      type: 'wasmfs-opfs-test-move-interrupt',
    });
    await new Promise(() => {});
  },
#endif

#if WASMFS_OPFS_TEST_CLOSE_FAILURE
  // This state and its witness channel live only in the OPFS ProxyWorker for
  // the focused close-failure test. The first injected failure happens before
  // the browser's close() call, so its native access handle remains live.
  $wasmfsOPFSTestCloseFailureState: {
    channel: undefined,
    injected: false,
    tracedNextAccess: false,
  },
  $wasmfsOPFSTestCloseFailureTrace__deps: ['$wasmfsOPFSTestCloseFailureState'],
  $wasmfsOPFSTestCloseFailureTrace: (phase, accessID) => {
    if (!wasmfsOPFSTestCloseFailureState.channel) {
      wasmfsOPFSTestCloseFailureState.channel = new BroadcastChannel(
        'wasmfs-opfs-test-close-failure');
    }
    wasmfsOPFSTestCloseFailureState.channel.postMessage({
      accessID,
      phase,
      type: 'wasmfs-opfs-test-close-failure',
    });
  },
#endif

#if WASMFS_OPFS_TEST_FILE_HANDLE_CACHE
  // This test-only trace observes strong FileSystemFileHandle references in
  // the OPFS ProxyWorker. It deliberately says nothing about directory,
  // Blob, SyncAccessHandle, browser-wide, or persistence capacity.
  $wasmfsOPFSTestFileHandleCacheTraceState: {
    channel: undefined,
  },
  $wasmfsOPFSTestFileHandleCacheTrace__deps: [
    '$wasmfsOPFSTestFileHandleCacheTraceState',
  ],
  $wasmfsOPFSTestFileHandleCacheTrace: (phase, fileID) => {
    if (!wasmfsOPFSTestFileHandleCacheTraceState.channel) {
      wasmfsOPFSTestFileHandleCacheTraceState.channel = new BroadcastChannel(
        'wasmfs-opfs-test-file-handle-cache');
    }
    wasmfsOPFSTestFileHandleCacheTraceState.channel.postMessage({
      fileID,
      phase,
      type: 'wasmfs-opfs-test-file-handle-cache',
    });
  },
#endif

#if !PTHREADS
  // OPFS will only be used on modern browsers that supports JS classes.
  $FileSystemAsyncAccessHandle: class {
    // This class implements the same interface as the sync version, but has
    // async reads and writes. Hopefully this will one day be implemented by the
    // platform so we can remove it.
    constructor(handle) {
      this.handle = handle;
    }
    async close() {}
    async flush() {}
    async getSize() {
      let file = await this.handle.getFile();
      return file.size;
    }
    async read(buffer, options = { at: 0 }) {
      let file = await this.handle.getFile();
      // The end position may be past the end of the file, but slice truncates
      // it.
      let slice = await file.slice(options.at, options.at + buffer.length);
      let fileBuffer = await slice.arrayBuffer();
      let array = new Uint8Array(fileBuffer);
      buffer.set(array);
      return array.length;
    }
    async write(buffer, options = { at: 0 }) {
      let writable = await this.handle.createWritable({keepExistingData: true});
      await writable.write({ type: 'write', position: options.at, data: buffer });
      await writable.close();
      return buffer.length;
    }
    async truncate(size) {
      let writable = await this.handle.createWritable({keepExistingData: true});
      await writable.truncate(size);
      await writable.close();
    }
  },

  $wasmfsOPFSCreateAsyncAccessHandle__deps: ['$FileSystemAsyncAccessHandle'],
  $wasmfsOPFSCreateAsyncAccessHandle: (fileHandle) => new FileSystemAsyncAccessHandle(fileHandle),
#endif

#if PTHREADS
  $wasmfsOPFSProxyFinish__deps: ['emscripten_proxy_finish'],
#endif
  $wasmfsOPFSProxyFinish: (ctx) => {
    // When using pthreads the proxy needs to know when the work is finished.
    // When used with JSPI the work will be executed in an async block so there
    // is no need to notify when done.
#if PTHREADS
    _emscripten_proxy_finish(ctx);
#endif
  },

  _wasmfs_opfs_acquire_profile_lease__deps: [
    '$wasmfsOPFSProfileLease',
    '$wasmfsOPFSProxyFinish',
  ],
  _wasmfs_opfs_acquire_profile_lease__async: 'auto',
  _wasmfs_opfs_acquire_profile_lease: async (ctx, profileNamePtr, errPtr) => {
    let err = 0;
    try {
      if (typeof navigator == 'undefined' || !navigator.locks ||
          typeof navigator.locks.request != 'function') {
        err = -{{{ cDefs.ENOSYS }}};
      } else if (wasmfsOPFSProfileLease.release ||
                 wasmfsOPFSProfileLease.request) {
        err = -{{{ cDefs.EBUSY }}};
      } else {
        let resolveAcquisition;
        let acquisitionSettled = false;
        let acquisition = new Promise((resolve) => {
          resolveAcquisition = resolve;
        });
        let settleAcquisition = (result) => {
          if (!acquisitionSettled) {
            acquisitionSettled = true;
            resolveAcquisition(result);
          }
        };
        let release;
        let released = new Promise((resolve) => {
          release = resolve;
        });
        let lockName = 'emscripten.wasmfs.opfs-profile/' +
                       UTF8ToString(profileNamePtr);
        let request = navigator.locks.request(
          lockName, {mode: 'exclusive', ifAvailable: true}, async (lock) => {
            if (!lock) {
              settleAcquisition(-{{{ cDefs.EBUSY }}});
              return;
            }
            wasmfsOPFSProfileLease.release = release;
            settleAcquisition(0);
            try {
              await released;
            } finally {
              wasmfsOPFSProfileLease.release = undefined;
              wasmfsOPFSProfileLease.request = undefined;
            }
          });
        wasmfsOPFSProfileLease.request = request;
        request.catch(() => {
          settleAcquisition(-{{{ cDefs.EIO }}});
        });
        err = await acquisition;
        if (err != 0) {
          wasmfsOPFSProfileLease.request = undefined;
        }
      }
    } catch {
      err = -{{{ cDefs.EIO }}};
    }
    {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_release_profile_lease__deps: [
    '$wasmfsOPFSProfileLease',
    '$wasmfsOPFSProxyFinish',
  ],
  _wasmfs_opfs_release_profile_lease__async: 'auto',
  _wasmfs_opfs_release_profile_lease: async (ctx, errPtr) => {
    let err = 0;
    let release = wasmfsOPFSProfileLease.release;
    let request = wasmfsOPFSProfileLease.request;
    if (!release || !request) {
      err = -{{{ cDefs.EIO }}};
    } else {
      release();
      try {
        await request;
      } catch {
        err = -{{{ cDefs.EIO }}};
      }
    }
    {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_init_root_directory__deps: ['$wasmfsOPFSDirectoryHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_init_root_directory__async: 'auto',
  _wasmfs_opfs_init_root_directory: async (ctx) => {
    // HandleAllocator reserves 0, so this first push puts the root in
    // permanent slot 1. It remains live for this backend's ProxyWorker.
    if (wasmfsOPFSDirectoryHandles.allocated.length == 1) {
      // Closure compiler errors on this as it does not recognize the OPFS
      // API yet, it seems. Unfortunately an existing annotation for this is in
      // the closure compiler codebase, and cannot be overridden in user code
      // (it complains on a duplicate type annotation), so just suppress it.
      /** @suppress {checkTypes} */
      let root = await navigator.storage.getDirectory();
      wasmfsOPFSDirectoryHandles.allocated.push(root);
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  // Look up the file with `name` under `parent`, creating it if requested.
  // The C++ OPFSFile wrapper acquires a FileSystemFileHandle only when an
  // operation needs it, so a metadata-only lookup does not retain one.
  $wasmfsOPFSGetOrCreateFile__deps: ['$wasmfsOPFSDirectoryHandles'],
  $wasmfsOPFSGetOrCreateFile: async (parent, name, create) => {
    let parentHandle = wasmfsOPFSDirectoryHandles.get(parent);
    try {
      await parentHandle.getFileHandle(name, {create: create});
    } catch (e) {
      if (e.name === "NotFoundError") {
        return -{{{ cDefs.EEXIST }}};
      }
      if (e.name === "TypeMismatchError") {
        return -{{{ cDefs.EISDIR }}};
      }
#if ASSERTIONS
      err('unexpected error:', e, e.stack);
#endif
      return -{{{ cDefs.EIO }}};
    }
    return 0;
  },

  // Return the file ID for the directory with `name` under `parent`, creating
  // it if it doesn't exist and `create` or otherwise return a negative error
  // code corresponding to the error.
  $wasmfsOPFSGetOrCreateDir__deps: ['$wasmfsOPFSDirectoryHandles'],
  $wasmfsOPFSGetOrCreateDir: async (parent, name, create) => {
    let parentHandle = wasmfsOPFSDirectoryHandles.get(parent);
    let childHandle;
    try {
      childHandle =
          await parentHandle.getDirectoryHandle(name, {create: create});
    } catch (e) {
      if (e.name === "NotFoundError") {
        return -{{{ cDefs.EEXIST }}};
      }
      if (e.name === "TypeMismatchError") {
        return -{{{ cDefs.ENOTDIR }}};
      }
#if ASSERTIONS
      err('unexpected error:', e, e.stack);
#endif
      return -{{{ cDefs.EIO }}};
    }
    return wasmfsOPFSDirectoryHandles.allocate(childHandle);
  },

  _wasmfs_opfs_get_child__deps: ['$wasmfsOPFSGetOrCreateFile',
                                 '$wasmfsOPFSGetOrCreateDir', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_get_child__async: 'auto',
  _wasmfs_opfs_get_child: async (ctx, parent, namePtr, childTypePtr, childIDPtr) => {
    let name = UTF8ToString(namePtr);
    let childType = 1;
    let childID = await wasmfsOPFSGetOrCreateFile(parent, name, false);
    if (childID == -{{{ cDefs.EISDIR }}}) {
      childType = 2;
      childID = await wasmfsOPFSGetOrCreateDir(parent, name, false);
    }
    {{{ makeSetValue('childTypePtr', 0, 'childType', 'i32') }}};
    {{{ makeSetValue('childIDPtr', 0, 'childID', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_get_entries__deps: [
    '$wasmfsOPFSProxyFinish',
    '$stackSave',
    '$stackRestore',
    '_wasmfs_opfs_record_entry',
  ],
  _wasmfs_opfs_get_entries__async: 'auto',
  _wasmfs_opfs_get_entries: async (ctx, dirID, entriesPtr, errPtr) => {
    let dirHandle = wasmfsOPFSDirectoryHandles.get(dirID);

    // TODO: Use 'for await' once Acorn supports that.
    try {
      let iter = dirHandle.entries();
      for (let entry; entry = await iter.next(), !entry.done;) {
        let [name, child] = entry.value;
        let sp = stackSave();
        let namePtr = stringToUTF8OnStack(name);
        let type = child.kind == "file" ?
            {{{ cDefs['File::DataFileKind'] }}} :
            {{{ cDefs['File::DirectoryKind'] }}};
          __wasmfs_opfs_record_entry(entriesPtr, namePtr, type)
        stackRestore(sp);
      }
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_insert_file__deps: ['$wasmfsOPFSGetOrCreateFile', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_insert_file__async: 'auto',
  _wasmfs_opfs_insert_file: async (ctx, parent, namePtr, childIDPtr) => {
    let name = UTF8ToString(namePtr);
    let childID = await wasmfsOPFSGetOrCreateFile(parent, name, true);
    {{{ makeSetValue('childIDPtr', 0, 'childID', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_acquire_file__deps: [
    '$wasmfsOPFSDirectoryHandles',
    '$wasmfsOPFSFileHandles',
#if WASMFS_OPFS_TEST_FILE_HANDLE_CACHE
    '$wasmfsOPFSTestFileHandleCacheTrace',
#endif
    '$wasmfsOPFSProxyFinish',
  ],
  _wasmfs_opfs_acquire_file__async: 'auto',
  _wasmfs_opfs_acquire_file: async (ctx, parent, namePtr, fileIDPtr) => {
    let fileID;
    try {
      let name = UTF8ToString(namePtr);
      let parentHandle = wasmfsOPFSDirectoryHandles.get(parent);
      let fileHandle = await parentHandle.getFileHandle(name);
      fileID = wasmfsOPFSFileHandles.allocate(fileHandle);
#if WASMFS_OPFS_TEST_FILE_HANDLE_CACHE
      wasmfsOPFSTestFileHandleCacheTrace('acquire', fileID);
#endif
    } catch (e) {
      if (e.name === 'NotFoundError') {
        fileID = -{{{ cDefs.ENOENT }}};
      } else if (e.name === 'TypeMismatchError') {
        fileID = -{{{ cDefs.EISDIR }}};
      } else {
#if ASSERTIONS
        err('unexpected error:', e, e.stack);
#endif
        fileID = -{{{ cDefs.EIO }}};
      }
    }
    {{{ makeSetValue('fileIDPtr', 0, 'fileID', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_insert_directory__deps: ['$wasmfsOPFSGetOrCreateDir', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_insert_directory__async: 'auto',
  _wasmfs_opfs_insert_directory: async (ctx, parent, namePtr, childIDPtr) => {
    let name = UTF8ToString(namePtr);
    let childID = await wasmfsOPFSGetOrCreateDir(parent, name, true);
    {{{ makeSetValue('childIDPtr', 0, 'childID', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_move_file__deps: ['$wasmfsOPFSFileHandles',
                                 '$wasmfsOPFSDirectoryHandles',
#if WASMFS_OPFS_TEST_MOVE_INTERRUPT
                                 '$wasmfsOPFSTestMoveInterrupt',
#endif
                                 '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_move_file__async: 'auto',
  _wasmfs_opfs_move_file: async (ctx, fileID, newParentID, namePtr, errPtr) => {
    let name = UTF8ToString(namePtr);
    let fileHandle = wasmfsOPFSFileHandles.get(fileID);
    let newDirHandle = wasmfsOPFSDirectoryHandles.get(newParentID);
#if WASMFS_OPFS_TEST_MOVE_INTERRUPT == 1
    await wasmfsOPFSTestMoveInterrupt('before');
#endif
    try {
      await fileHandle.move(newDirHandle, name);
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
      wasmfsOPFSProxyFinish(ctx);
      return;
    }
#if WASMFS_OPFS_TEST_MOVE_INTERRUPT == 2
    // Keep this outside the move error mapping. A test-hook failure must not
    // become a backend EIO result.
    await wasmfsOPFSTestMoveInterrupt('after');
#endif
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_remove_child__deps: ['$wasmfsOPFSDirectoryHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_remove_child__async: 'auto',
  _wasmfs_opfs_remove_child: async (ctx, dirID, namePtr, errPtr) => {
    let name = UTF8ToString(namePtr);
    let dirHandle = wasmfsOPFSDirectoryHandles.get(dirID);
    try {
      await dirHandle.removeEntry(name);
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_free_file__deps: ['$wasmfsOPFSFileHandles',
#if WASMFS_OPFS_TEST_FILE_HANDLE_CACHE
                                  '$wasmfsOPFSTestFileHandleCacheTrace',
#endif
                                 ],
  _wasmfs_opfs_free_file: (fileID) => {
    wasmfsOPFSFileHandles.free(fileID);
#if WASMFS_OPFS_TEST_FILE_HANDLE_CACHE
    wasmfsOPFSTestFileHandleCacheTrace('release', fileID);
#endif
  },

  _wasmfs_opfs_free_directory__deps: ['$wasmfsOPFSDirectoryHandles'],
  _wasmfs_opfs_free_directory: (dirID) => {
    wasmfsOPFSDirectoryHandles.free(dirID);
  },

  _wasmfs_opfs_open_access__deps: ['$wasmfsOPFSFileHandles',
                                   '$wasmfsOPFSAccessHandles', '$wasmfsOPFSProxyFinish',
#if WASMFS_OPFS_TEST_CLOSE_FAILURE
                                   '$wasmfsOPFSTestCloseFailureState',
                                   '$wasmfsOPFSTestCloseFailureTrace',
#endif
#if !PTHREADS
                                   '$wasmfsOPFSCreateAsyncAccessHandle'
#endif
                                  ],
  _wasmfs_opfs_open_access__async: 'auto',
  _wasmfs_opfs_open_access: async (ctx, fileID, accessIDPtr) => {
    let fileHandle = wasmfsOPFSFileHandles.get(fileID);
    let accessID;
    try {
      let accessHandle;
#if PTHREADS
      // TODO: Remove this once the Access Handles API has settled.
      // TODO: Closure is confused by this code that supports two versions of
      //       the same API, so suppress type checking on it.
      /** @suppress {checkTypes} */
      var len = FileSystemFileHandle.prototype.createSyncAccessHandle.length;
      if (len == 0) {
        accessHandle = await fileHandle.createSyncAccessHandle();
      } else {
        accessHandle = await fileHandle.createSyncAccessHandle(
            {mode: "in-place"});
      }
#else
      accessHandle = await wasmfsOPFSCreateAsyncAccessHandle(fileHandle);
#endif
      accessID = wasmfsOPFSAccessHandles.allocate(accessHandle);
#if WASMFS_OPFS_TEST_CLOSE_FAILURE
      if (wasmfsOPFSTestCloseFailureState.injected &&
          !wasmfsOPFSTestCloseFailureState.tracedNextAccess) {
        wasmfsOPFSTestCloseFailureState.tracedNextAccess = true;
        wasmfsOPFSTestCloseFailureTrace('next-access', accessID);
      }
#endif
    } catch (e) {
      // TODO: Presumably only one of these will appear in the final API?
      if (e.name === "InvalidStateError" ||
          e.name === "NoModificationAllowedError") {
        accessID = -{{{ cDefs.EACCES }}};
      } else {
#if ASSERTIONS
        err('unexpected error:', e, e.stack);
#endif
        accessID = -{{{ cDefs.EIO }}};
      }
    }
    {{{ makeSetValue('accessIDPtr', 0, 'accessID', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_open_blob__deps: ['$wasmfsOPFSFileHandles',
                                 '$wasmfsOPFSBlobs', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_open_blob__async: 'auto',
  _wasmfs_opfs_open_blob: async (ctx, fileID, blobIDPtr) => {
    let fileHandle = wasmfsOPFSFileHandles.get(fileID);
    let blobID;
    try {
      let blob = await fileHandle.getFile();
      blobID = wasmfsOPFSBlobs.allocate(blob);
    } catch (e) {
      if (e.name === "NotAllowedError") {
        blobID = -{{{ cDefs.EACCES }}};
      } else {
#if ASSERTIONS
        err('unexpected error:', e, e.stack);
#endif
        blobID = -{{{ cDefs.EIO }}};
      }
    }
    {{{ makeSetValue('blobIDPtr', 0, 'blobID', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_close_access__deps: ['$wasmfsOPFSAccessHandles', '$wasmfsOPFSProxyFinish',
#if WASMFS_OPFS_TEST_CLOSE_FAILURE
                                     '$wasmfsOPFSTestCloseFailureState',
                                     '$wasmfsOPFSTestCloseFailureTrace',
#endif
                                    ],
  _wasmfs_opfs_close_access__async: 'auto',
  _wasmfs_opfs_close_access: async (ctx, accessID, errPtr) => {
    let accessHandle = wasmfsOPFSAccessHandles.get(accessID);
    let closed = false;
    try {
#if WASMFS_OPFS_TEST_CLOSE_FAILURE
      if (!wasmfsOPFSTestCloseFailureState.injected) {
        wasmfsOPFSTestCloseFailureState.injected = true;
        wasmfsOPFSTestCloseFailureTrace('close-rejected', accessID);
        throw new Error('injected OPFS access close failure');
      }
#endif
      await accessHandle.close();
      closed = true;
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    }
    // A rejected close leaves the browser resource's state ambiguous. Keep the
    // slot strongly referenced and unavailable for reuse until the backend's
    // worker context is torn down.
    if (closed) {
      wasmfsOPFSAccessHandles.free(accessID);
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_close_blob__deps: ['$wasmfsOPFSBlobs'],
  _wasmfs_opfs_close_blob: (blobID) => {
    wasmfsOPFSBlobs.free(blobID);
  },

  _wasmfs_opfs_read_access__i53abi: true,
  _wasmfs_opfs_read_access__deps: ['$wasmfsOPFSAccessHandles'],
  _wasmfs_opfs_read_access__async: 'auto',
  _wasmfs_opfs_read_access: {{{ asyncIf(!PTHREADS) }}}(accessID, bufPtr, len, pos) => {
    let accessHandle = wasmfsOPFSAccessHandles.get(accessID);
    let data = HEAPU8.subarray(bufPtr, bufPtr + len);
    try {
      return {{{ awaitIf(!PTHREADS) }}}accessHandle.read(data, {at: pos});
    } catch (e) {
      if (e.name == "TypeError") {
        return -{{{ cDefs.EINVAL }}};
      }
#if ASSERTIONS
      err('unexpected error:', e, e.stack);
#endif
      return -{{{ cDefs.EIO }}};
    }
  },

  _wasmfs_opfs_read_blob__i53abi: true,
  _wasmfs_opfs_read_blob__deps: ['$wasmfsOPFSBlobs', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_read_blob__async: 'auto',
  _wasmfs_opfs_read_blob: async (ctx, blobID, bufPtr, len, pos, nreadPtr) => {
    let blob = wasmfsOPFSBlobs.get(blobID);
    let slice = blob.slice(pos, pos + len);
    let nread = 0;

    try {
      // TODO: Use ReadableStreamBYOBReader once
      // https://bugs.chromium.org/p/chromium/issues/detail?id=1189621 is
      // resolved.
      let buf = await slice.arrayBuffer();
      let data = new Uint8Array(buf);
      HEAPU8.set(data, bufPtr);
      nread += data.length;
    } catch (e) {
      if (e instanceof RangeError) {
        nread = -{{{ cDefs.EFAULT }}};
      } else {
#if ASSERTIONS
        err('unexpected error:', e, e.stack);
#endif
        nread = -{{{ cDefs.EIO }}};
      }
    }

    {{{ makeSetValue('nreadPtr', 0, 'nread', 'i32') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_write_access__i53abi: true,
  _wasmfs_opfs_write_access__deps: ['$wasmfsOPFSAccessHandles'],
  _wasmfs_opfs_write_access__async: 'auto',
  _wasmfs_opfs_write_access: {{{ asyncIf(!PTHREADS) }}}(accessID, bufPtr, len, pos) => {
    let accessHandle = wasmfsOPFSAccessHandles.get(accessID);
    let data = HEAPU8.subarray(bufPtr, bufPtr + len);
    try {
      return {{{ awaitIf(!PTHREADS) }}}accessHandle.write(data, {at: pos});
    } catch (e) {
      if (e.name == "TypeError") {
        return -{{{ cDefs.EINVAL }}};
      }
#if ASSERTIONS
      err('unexpected error:', e, e.stack);
#endif
      return -{{{ cDefs.EIO }}};
    }
  },

  _wasmfs_opfs_get_size_access__deps: ['$wasmfsOPFSAccessHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_get_size_access__async: 'auto',
  _wasmfs_opfs_get_size_access: async (ctx, accessID, sizePtr) => {
    let accessHandle = wasmfsOPFSAccessHandles.get(accessID);
    let size;
    try {
      size = await accessHandle.getSize();
    } catch {
      size = -{{{ cDefs.EIO }}};
    }
    {{{ makeSetValue('sizePtr', 0, 'size', 'i64') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_get_size_blob__i53abi: true,
  _wasmfs_opfs_get_size_blob__deps: ['$wasmfsOPFSBlobs'],
  _wasmfs_opfs_get_size_blob: (blobID) => {
    // This cannot fail.
	  return wasmfsOPFSBlobs.get(blobID).size;
  },

  _wasmfs_opfs_get_size_file__deps: ['$wasmfsOPFSFileHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_get_size_file__async: 'auto',
  _wasmfs_opfs_get_size_file: async (ctx, fileID, sizePtr) => {
    let fileHandle = wasmfsOPFSFileHandles.get(fileID);
    let size;
    try {
      size = (await fileHandle.getFile()).size;
    } catch {
      size = -{{{ cDefs.EIO }}};
    }
    {{{ makeSetValue('sizePtr', 0, 'size', 'i64') }}};
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_set_size_access__i53abi: true,
  _wasmfs_opfs_set_size_access__deps: ['$wasmfsOPFSAccessHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_set_size_access__async: 'auto',
  _wasmfs_opfs_set_size_access: async (ctx, accessID, size, errPtr) => {
    let accessHandle = wasmfsOPFSAccessHandles.get(accessID);
    try {
      await accessHandle.truncate(size);
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_set_size_file__i53abi: true,
  _wasmfs_opfs_set_size_file__deps: ['$wasmfsOPFSFileHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_set_size_file__async: 'auto',
  _wasmfs_opfs_set_size_file: async (ctx, fileID, size, errPtr) => {
    let fileHandle = wasmfsOPFSFileHandles.get(fileID);
    try {
      let writable = await fileHandle.createWritable({keepExistingData: true});
      await writable.truncate(size);
      await writable.close();
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_opfs_flush_access__deps: ['$wasmfsOPFSAccessHandles', '$wasmfsOPFSProxyFinish'],
  _wasmfs_opfs_flush_access__async: 'auto',
  _wasmfs_opfs_flush_access: async (ctx, accessID, errPtr) => {
    let accessHandle = wasmfsOPFSAccessHandles.get(accessID);
    try {
      await accessHandle.flush();
    } catch {
      let err = -{{{ cDefs.EIO }}};
      {{{ makeSetValue('errPtr', 0, 'err', 'i32') }}};
    }
    wasmfsOPFSProxyFinish(ctx);
  }
});
