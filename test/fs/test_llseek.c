/*
 * Copyright 2018 The Emscripten Authors.  All rights reserved.
 * Emscripten is available under two separate licenses, the MIT license and the
 * University of Illinois/NCSA Open Source License.  Both these licenses can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <emscripten/emscripten.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

int main()
{
#ifdef WASMFS
  const off_t jsExactOffsetLimit = (off_t)INT64_C(1) << 53;
  int nativeFd = open("/dev/null", O_RDONLY);
  assert(nativeFd >= 0);
  assert(lseek(nativeFd, jsExactOffsetLimit + 1, SEEK_SET) ==
         jsExactOffsetLimit + 1);
  assert(close(nativeFd) == 0);
#endif

  EM_ASM(
    FS.writeFile('testfile', 'a=1\nb=2\n');
    var stream = FS.open('testfile', 'a');
    var fd = FS.write(stream, new Uint8Array([99, 61, 51]) /* c=3 */, 0, 3);

    // check invalid whence
    var ex;
    try {
      FS.llseek(stream, 0, 99);
    } catch(e) {
      ex = e;
    }
    assert(ex instanceof FS.ErrnoError && ex.errno === 28 /* EINVAL */);

    assert(FS.llseek(stream, 0, 1 /* SEEK_CUR */) === 11);
    assert(FS.llseek(stream, 0x80000000, 0 /* SEEK_SET */) === 0x80000000);
    assert(FS.llseek(stream, 0x100000007, 1 /* SEEK_CUR */) === 0x180000007);

    console.log("success");
    FS.close(stream);
  );

#ifdef WASMFS
  EM_ASM({
    var stream = FS.open('testfile', 'a');
    var exactOffsetLimit = 0x20000000000000;
    assert(FS.llseek(stream, exactOffsetLimit, 0 /* SEEK_SET */) === exactOffsetLimit);

    function expectOverflow(offset, whence) {
      var ex;
      try {
        FS.llseek(stream, offset, whence);
      } catch (e) {
        ex = e;
      }
      assert(ex instanceof FS.ErrnoError && ex.errno === 61 /* EOVERFLOW */);
    }

    // An invalid JS-facing result must not alter the native descriptor state.
    expectOverflow(exactOffsetLimit + 2, 0 /* SEEK_SET */);
    assert(FS.llseek(stream, 0, 1 /* SEEK_CUR */) === exactOffsetLimit);
    expectOverflow(1, 1 /* SEEK_CUR */);
    assert(FS.llseek(stream, -exactOffsetLimit, 1 /* SEEK_CUR */) === 0);
    FS.close(stream);
  });
#endif

  return 0;
}
