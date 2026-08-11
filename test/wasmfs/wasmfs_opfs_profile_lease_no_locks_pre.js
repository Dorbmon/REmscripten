// Copyright 2026 The Emscripten Authors.  All rights reserved.
// Emscripten is available under two separate licenses, the MIT license and the
// University of Illinois/NCSA Open Source License.  Both these licenses can be
// found in the LICENSE file.

// This pre-js runs in every Node pthread worker too. Hide the experimental
// Node implementation so the C test covers the deterministic no-Web-Locks
// error path used by browsers that do not expose WorkerNavigator.locks.
if (typeof navigator != 'undefined') {
  Object.defineProperty(navigator, 'locks', {
    configurable: true,
    value: undefined,
  });
}
