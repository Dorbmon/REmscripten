// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// Trace OPFS root requests from every test realm, including the OPFS dedicated
// worker. The browser test checks that a failed `ifAvailable` contender emits
// no request before it returns EBUSY.
if (typeof BroadcastChannel != 'undefined' &&
    typeof navigator != 'undefined' && navigator.storage &&
    typeof navigator.storage.getDirectory == 'function') {
  const channel = new BroadcastChannel('wasmfs-opfs-profile-lease-trace');
  const storage = navigator.storage;
  const getDirectory = storage.getDirectory.bind(storage);
  Object.defineProperty(storage, 'getDirectory', {
    configurable: true,
    value: (...args) => {
      channel.postMessage({type: 'opfs-root-request'});
      return getDirectory(...args);
    },
  });
}
