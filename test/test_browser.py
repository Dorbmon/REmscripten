# Copyright 2013 The Emscripten Authors.  All rights reserved.
# Emscripten is available under two separate licenses, the MIT license and the
# University of Illinois/NCSA Open Source License.  Both these licenses can be
# found in the LICENSE file.

import argparse
import os
import random
import re
import shlex
import shutil
import struct
import subprocess
import time
import zlib
from functools import wraps
from http.server import (
  BaseHTTPRequestHandler,
  HTTPServer,
  SimpleHTTPRequestHandler,
  ThreadingHTTPServer,
)
from pathlib import Path
from urllib.request import urlopen

import common
from browser_common import (
  CHROMIUM_BASED_BROWSERS,
  BrowserCore,
  HttpServerThread,
  Reporting,
  browser_should_skip_feature,
  find_browser_test_file,
  get_browser,
  get_safari_version,
  has_browser,
  is_chrome,
  is_firefox,
  is_safari,
)
from common import (
  EMRUN,
  WEBIDL_BINDER,
  RunnerCore,
  copy_asset,
  copytree,
  create_file,
  ensure_dir,
  path_from_root,
  read_file,
  test_file,
)
from decorators import (
  also_with_asan,
  also_with_fetch_streaming,
  also_with_minimal_runtime,
  also_with_wasm2js,
  also_with_wasmfs,
  disabled,
  flaky,
  no_2gb,
  no_4gb,
  no_wasm64,
  parameterize,
  parameterized,
  requires_dev_dependency,
  requires_wasm2js,
  skip_if,
  skip_if_simple,
  with_all_sjlj,
)

from tools import ports, shared, utils
from tools.feature_matrix import Feature
from tools.link import binary_encode
from tools.shared import EMCC, FILE_PACKAGER, PIPE
from tools.utils import WINDOWS, delete_dir, write_binary, write_file


def make_test_chunked_synchronous_xhr_server(support_byte_ranges, data, port):
  class ChunkedServerHandler(BaseHTTPRequestHandler):
    num_get_connections = 0

    def sendheaders(s, extra=None, length=None):
      length = length or len(data)
      s.send_response(200)
      s.send_header("Content-Length", str(length))
      s.send_header("Access-Control-Allow-Origin", "http://localhost:%s" % port)
      s.send_header('Cross-Origin-Resource-Policy', 'cross-origin')

      s.send_header('Cache-Control', 'no-cache, no-store, must-revalidate, private, max-age=0')
      s.send_header('Expires', '0')
      s.send_header('Pragma', 'no-cache')
      s.send_header('Vary', '*') # Safari insists on caching if this header is not present in addition to the above

      s.send_header("Access-Control-Expose-Headers", "Content-Length, Accept-Ranges")
      s.send_header("Content-type", "application/octet-stream")
      if support_byte_ranges:
        s.send_header("Accept-Ranges", "bytes")
      if extra:
        for key, value in extra:
          s.send_header(key, value)
      s.end_headers()

    def do_HEAD(s):
      s.sendheaders()

    def do_OPTIONS(s):
      s.sendheaders([("Access-Control-Allow-Headers", "Range")], 0)

    def do_GET(s):
      # CORS preflight makes OPTIONS requests which we need to account for.
      expectedConns = 22
      s.num_get_connections += 1
      assert s.num_get_connections < expectedConns

      if s.path == '/':
        s.sendheaders()
      elif not support_byte_ranges:
        s.sendheaders()
        s.wfile.write(data)
      else:
        start, end = s.headers.get("range").split("=")[1].split("-")
        start = int(start)
        end = int(end)
        end = min(len(data) - 1, end)
        length = end - start + 1
        s.sendheaders([], length)
        s.wfile.write(data[start:end + 1])

  return HTTPServer(('localhost', 11111), ChunkedServerHandler)


# This is similar to @core.no_wasmfs, but it disable WasmFS and runs the test
# normally. That is, in core we skip the test if we are in the wasmfs.* mode,
# while in browser we don't have such modes, so we force the test to run without
# WasmFS.
#
# When WasmFS is on by default, these annotations will still be needed. Only
# when we remove the old JS FS entirely would we remove them.
def no_wasmfs(note):
  assert not callable(note)

  def decorator(f):
    assert callable(f)

    @wraps(f)
    def decorated(self, *args, **kwargs):
      self.set_setting('WASMFS', 0)
      f(self, *args, **kwargs)
    return decorated
  return decorator


def shell_with_script(shell_file, output_file, replacement):
  shell = read_file(path_from_root('src', shell_file))
  create_file(output_file, shell.replace('{{{ SCRIPT }}}', replacement))


def is_swiftshader(_):
  return is_chrome() and '--use-gl=swiftshader' in get_browser()


no_swiftshader = skip_if_simple('not compatible with swiftshader', is_swiftshader)

no_chrome = skip_if('no_chrome', lambda _: is_chrome(), 'chrome is not supported')

no_firefox = skip_if('no_firefox', lambda _: is_firefox(), 'firefox is not supported')

no_safari = skip_if('no_safari', lambda _: is_safari(), 'safari is not supported')

only_chromium = skip_if_simple(
  'only_chromium', lambda _: not is_chrome(),
  'this test uses Chromium OPFS move interruption behavior')


def requires_version(name, version_getter):
  assert callable(version_getter)

  def decorator(min_required_version, note=''):
    return skip_if_simple(name, lambda _: version_getter() < min_required_version, f'{name} v{version_getter()} is not supported (need v{min_required_version} at minimum) {note}')

  return decorator


requires_safari_version = requires_version('safari', get_safari_version)


def is_jspi(args):
  return '-sJSPI' in args


def also_with_threads(f):
  assert callable(f)

  @wraps(f)
  def decorated(self, threads, *args, **kwargs):
    if threads:
      self.cflags += ['-pthread']
    f(self, *args, **kwargs)

  parameterize(decorated, {'': (False,),
                           'pthreads': (True,)})

  return decorated


def also_with_proxy_to_pthread(f):
  assert callable(f)

  @wraps(f)
  def decorated(self, threads, *args, **kwargs):
    if threads:
      self.cflags += ['-pthread', '-sPROXY_TO_PTHREAD']
    f(self, *args, **kwargs)

  parameterize(decorated, {'': (False,),
                           'proxy_to_pthread': (True,)})

  return decorated


def skipIfFeatureNotAvailable(skip_env_var, feature, message):
  for env_var in skip_env_var if type(skip_env_var) == list else [skip_env_var]:
    should_skip = browser_should_skip_feature(env_var, feature)
    if should_skip:
      break

  def decorator(f):
    assert callable(f)

    @wraps(f)
    def decorated(self, *args, **kwargs):
      if should_skip == 'error':
        raise Exception(f'This test requires a browser that supports {feature.name} but your browser {get_browser()} does not support this. Run with {skip_env_var}=1 or EMTEST_AUTOSKIP=1 to skip this test automatically.')
      elif should_skip and bool(re.search(r"MIN_.*_VERSION", os.getenv('EMCC_CFLAGS', ''))):
        # should_skip=True, so we should skip running this test. However, user has specified a MIN_x_VERSION
        # directive in EMCC_CFLAGS, so we cannot even try to compile this test, or otherwise emcc can
        # error out on the MIN_x_VERSION being too old. So skip both compiling+running this test.
        self.skipTest(message)
      elif should_skip:
        # Skip running this test in a browser, but do test compiling it, to get partial coverage.
        self.skip_exec = message

      f(self, *args, **kwargs)

    return decorated

  return decorator


def webgl2_disabled():
  return browser_should_skip_feature('EMTEST_LACKS_WEBGL2', Feature.WEBGL2) or browser_should_skip_feature('EMTEST_LACKS_GRAPHICS_HARDWARE', Feature.WEBGL2)


requires_graphics_hardware = skipIfFeatureNotAvailable('EMTEST_LACKS_GRAPHICS_HARDWARE', None, 'This test requires graphics hardware')
requires_webgl2 = skipIfFeatureNotAvailable(['EMTEST_LACKS_WEBGL2', 'EMTEST_LACKS_GRAPHICS_HARDWARE'], Feature.WEBGL2, 'This test requires WebGL2 to be available')
requires_webgpu = skipIfFeatureNotAvailable(['EMTEST_LACKS_WEBGPU', 'EMTEST_LACKS_GRAPHICS_HARDWARE'], Feature.WEBGPU, 'This test requires WebGPU to be available')
requires_sound_hardware = skipIfFeatureNotAvailable('EMTEST_LACKS_SOUND_HARDWARE', None, 'This test requires sound hardware')
requires_microphone_access = skipIfFeatureNotAvailable('EMTEST_LACKS_MICROPHONE_ACCESS', None, 'This test accesses microphone, which may need accepting a user prompt to enable it.')
requires_offscreen_canvas = skipIfFeatureNotAvailable('EMTEST_LACKS_OFFSCREEN_CANVAS', Feature.OFFSCREENCANVAS_SUPPORT, 'This test requires a browser with OffscreenCanvas')
requires_es6_workers = skipIfFeatureNotAvailable('EMTEST_LACKS_ES6_WORKERS', Feature.WORKER_ES6_MODULES, 'This test requires a browser with ES6 Module Workers support')
requires_growable_arraybuffers = skipIfFeatureNotAvailable('EMTEST_LACKS_GROWABLE_ARRAYBUFFERS', Feature.GROWABLE_ARRAYBUFFERS, 'This test requires a browser that supports growable ArrayBuffers')
# N.b. not all SharedArrayBuffer requiring tests are annotated with this decorator, since at this point there are so many of such tests.
# As a middle ground, if a test has a name 'thread' or 'wasm_worker' in it, then it does not need decorating. To run all single-threaded tests in
# the suite, one can run "EMTEST_LACKS_SHARED_ARRAY_BUFFER=1 test/runner browser skip:browser.test_*thread* skip:browser.test_*wasm_worker* skip:browser.test_*audio_worklet*"
requires_shared_array_buffer = skipIfFeatureNotAvailable('EMTEST_LACKS_SHARED_ARRAY_BUFFER', Feature.THREADS, 'This test requires a browser with SharedArrayBuffer support')


class browser(BrowserCore):
  @classmethod
  def setUpClass(cls):
    super().setUpClass()
    cls.browser_timeout = 60
    if get_browser() != 'node':
      print()
      print('Running the browser tests. Make sure the browser allows popups from localhost.')
      print()

  def require_jspi(self):
    if not is_chrome():
      self.skipTest(f'Current browser ({get_browser()}) does not support JSPI. Only chromium-based browsers ({CHROMIUM_BASED_BROWSERS}) support JSPI today.')
    super().require_jspi()

  def post_manual_reftest(self):
    assert os.path.exists('reftest.js')
    self.add_browser_reporting()
    html = read_file('test.html')
    html = html.replace('</body>', '''
<script src="browser_reporting.js"></script>
<script src="reftest.js"></script>
<script>
var windowClose = window.close;
window.close = () => {
  // wait for rafs to arrive and the screen to update before reftesting
  setTimeout(() => {
    doReftest();
    setTimeout(windowClose, 5000);
  }, 1000);
};
</script>
</body>''')
    create_file('test.html', html)

  def make_reftest(self, expected):
    # make sure the pngs used here have no color correction, using e.g.
    #   pngcrush -rem gAMA -rem cHRM -rem iCCP -rem sRGB infile outfile
    shutil.copy(expected, 'expected.png')
    create_file('reftest.js', f'''
      const reftestRebaseline = {common.EMTEST_REBASELINE};
    ''' + read_file(test_file('reftest.js')))

  def reftest(self, filename, reference=None, reference_slack=0, *args, **kwargs):
    """Special case of `btest` that uses reference image
    """
    if not reference:
      reference = utils.replace_suffix(filename, '.png')
    reference = find_browser_test_file(reference)
    assert 'expected' not in kwargs
    expected = [str(i) for i in range(reference_slack + 1)]
    self.make_reftest(reference)
    kwargs.setdefault('cflags', [])
    kwargs['cflags'] += ['--pre-js', 'reftest.js', '-sGL_TESTING']

    try:
      return self.btest(filename, expected=expected, *args, **kwargs)
    finally:
      if common.EMTEST_REBASELINE and os.path.exists('actual.png'):
        print(f'overwriting expected image: {reference}')
        self.run_process('pngcrush -rem gAMA -rem cHRM -rem iCCP -rem sRGB actual.png'.split() + [reference])

  def test_sdl1_in_emscripten_nonstrict_mode(self):
    if 'EMCC_STRICT' in os.environ and int(os.environ['EMCC_STRICT']):
      self.skipTest('This test requires being run in non-strict mode (EMCC_STRICT env. variable unset)')
    # TODO: This test is verifying behavior that will be deprecated at some point in the future, remove this test once
    # system JS libraries are no longer automatically linked to anymore.
    self.reftest('hello_world_sdl.c', 'browser/htmltest.png')

  def test_sdl1(self):
    self.reftest('hello_world_sdl.c', 'browser/htmltest.png', cflags=['-lSDL', '-lGL'])
    self.reftest('hello_world_sdl.c', 'browser/htmltest.png', cflags=['-sUSE_SDL', '-lGL']) # is the default anyhow

  def test_sdl1_es6(self):
    self.reftest('hello_world_sdl.c', 'browser/htmltest.png', cflags=['-sUSE_SDL', '-lGL', '-sEXPORT_ES6'])

  @no_safari('TODO: Fails in NEW Safari 26.0.1 (21622.1.22.11.15), but not in OLD Safari 17.6 (17618.3.11.11.7, 17618) or Safari 18.5 (20621.2.5.11.8)')
  def test_emscripten_log(self):
    self.btest_exit('test_emscripten_log.cpp', cflags=['-Wno-deprecated-pragma', '-gsource-map', '-g2'])

  @also_with_wasmfs
  def test_preload_file(self):
    create_file('somefile.txt', 'load me right before running the code please')
    create_file('.somefile.txt', 'load me right before running the code please')
    create_file('some@file.txt', 'load me right before running the code please')

    absolute_src_path = os.path.abspath('somefile.txt')

    def make_main(path):
      print('make main at', path)
      path = path.replace('\\', '\\\\').replace('"', '\\"') # Escape tricky path name for use inside a C string.
      # TODO: change this when wasmfs supports relative paths.
      if self.get_setting('WASMFS'):
        path = "/" + path
      create_file('main.c', r'''
        #include <assert.h>
        #include <stdio.h>
        #include <string.h>
        #include <emscripten.h>
        int main() {
          FILE *f = fopen("%s", "r");
          char buf[100];
          fread(buf, 1, 20, f);
          buf[20] = 0;
          fclose(f);
          printf("|%%s|\n", buf);

          assert(strcmp("load me right before", buf) == 0);
          return 0;
        }
        ''' % path)

    test_cases = [
      # (source preload-file string, file on target FS to load)
      ("somefile.txt", "somefile.txt"),
      (".somefile.txt@somefile.txt", "somefile.txt"),
      ("./somefile.txt", "somefile.txt"),
      ("somefile.txt@file.txt", "file.txt"),
      ("./somefile.txt@file.txt", "file.txt"),
      ("./somefile.txt@./file.txt", "file.txt"),
      ("somefile.txt@/file.txt", "file.txt"),
      ("somefile.txt@/", "somefile.txt"),
      (absolute_src_path + "@file.txt", "file.txt"),
      (absolute_src_path + "@/file.txt", "file.txt"),
      (absolute_src_path + "@/", "somefile.txt"),
      ("somefile.txt@/directory/file.txt", "/directory/file.txt"),
      ("somefile.txt@/directory/file.txt", "directory/file.txt"),
      (absolute_src_path + "@/directory/file.txt", "directory/file.txt"),
      ("some@@file.txt@other.txt", "other.txt"),
      ("some@@file.txt@some@@otherfile.txt", "some@otherfile.txt")]

    for srcpath, dstpath in test_cases:
      print('Testing', srcpath, dstpath)
      make_main(dstpath)
      self.btest_exit('main.c', cflags=['--preload-file', srcpath])
    if WINDOWS:
      # On Windows, the following non-alphanumeric non-control code ASCII characters are supported.
      # The characters <, >, ", |, ?, * are not allowed, because the Windows filesystem doesn't support those.
      tricky_filename = '!#$%&\'()+,-. ;=@[]^_`{}~.txt'
    else:
      # All 7-bit non-alphanumeric non-control code ASCII characters except /, : and \ are allowed.
      tricky_filename = '!#$%&\'()+,-. ;=@[]^_`{}~ "*<>?|.txt'
    create_file(tricky_filename, 'load me right before running the code please')
    make_main(tricky_filename)
    # As an Emscripten-specific feature, the character '@' must be escaped in the form '@@' to not confuse with the 'src@dst' notation.
    self.btest_exit('main.c', cflags=['--preload-file', tricky_filename.replace('@', '@@')])

    # TODO: WASMFS doesn't support the rest of this test yet. Exit early.
    if self.get_setting('WASMFS'):
      return

    # By absolute path

    make_main('somefile.txt') # absolute becomes relative
    self.btest_exit('main.c', cflags=['--preload-file', absolute_src_path])

    # Test subdirectory handling with asset packaging.
    delete_dir('assets')
    ensure_dir('assets/sub/asset1')
    ensure_dir('assets/sub/asset1/.git') # Test adding directory that shouldn't exist.
    ensure_dir('assets/sub/asset2')
    create_file('assets/sub/asset1/file1.txt', '''load me right before running the code please''')
    create_file('assets/sub/asset1/.git/shouldnt_be_embedded.txt', '''this file should not get embedded''')
    create_file('assets/sub/asset2/file2.txt', '''load me right before running the code please''')
    absolute_assets_src_path = 'assets'

    def make_main_two_files(path1, path2, nonexistingpath):
      create_file('main.c', r'''
        #include <stdio.h>
        #include <assert.h>
        #include <string.h>
        #include <emscripten.h>
        int main() {
          FILE *f = fopen("%s", "r");
          char buf[100];
          fread(buf, 1, 20, f);
          buf[20] = 0;
          fclose(f);
          printf("|%%s|\n", buf);

          assert(strcmp("load me right before", buf) == 0);

          f = fopen("%s", "r");
          assert(f != NULL);
          fclose(f);

          f = fopen("%s", "r");
          assert(f == NULL);

          return 0;
        }
      ''' % (path1, path2, nonexistingpath))

    test_cases = [
      # (source directory to embed, file1 on target FS to load, file2 on target FS to load, name of a file that *shouldn't* exist on VFS)
      ("assets", "assets/sub/asset1/file1.txt", "assets/sub/asset2/file2.txt", "assets/sub/asset1/.git/shouldnt_be_embedded.txt"),
      ("assets/", "assets/sub/asset1/file1.txt", "assets/sub/asset2/file2.txt", "assets/sub/asset1/.git/shouldnt_be_embedded.txt"),
      ("assets@/", "/sub/asset1/file1.txt", "/sub/asset2/file2.txt", "/sub/asset1/.git/shouldnt_be_embedded.txt"),
      ("assets/@/", "/sub/asset1/file1.txt", "/sub/asset2/file2.txt", "/sub/asset1/.git/shouldnt_be_embedded.txt"),
      ("assets@./", "/sub/asset1/file1.txt", "/sub/asset2/file2.txt", "/sub/asset1/.git/shouldnt_be_embedded.txt"),
      (absolute_assets_src_path + "@/", "/sub/asset1/file1.txt", "/sub/asset2/file2.txt", "/sub/asset1/.git/shouldnt_be_embedded.txt"),
      (absolute_assets_src_path + "@/assets", "/assets/sub/asset1/file1.txt", "/assets/sub/asset2/file2.txt", "assets/sub/asset1/.git/shouldnt_be_embedded.txt")]

    for test in test_cases:
      (srcpath, dstpath1, dstpath2, nonexistingpath) = test
      make_main_two_files(dstpath1, dstpath2, nonexistingpath)
      print(srcpath)
      self.btest_exit('main.c', cflags=['--preload-file', srcpath, '--exclude-file', '*/.*'])

    # Should still work with -o subdir/..

    make_main('somefile.txt') # absolute becomes relative
    ensure_dir('dirrey')
    self.compile_btest('main.c', ['--preload-file', absolute_src_path, '-o', 'dirrey/page.html'], reporting=Reporting.JS_ONLY)
    self.run_browser('dirrey/page.html', '/report_result?exit:0')

    # With FS.preloadFile

    create_file('pre.js', '''
      // we need --use-preload-plugins for this.
      Module.preRun = () => FS.createPreloadedFile('/', 'someotherfile.txt', 'somefile.txt', true, false);
    ''')
    make_main('someotherfile.txt')
    self.btest_exit('main.c', cflags=['--pre-js', 'pre.js', '--use-preload-plugins'])

  # Tests that user .html shell files can manually download .data files created with --preload-file cmdline.
  @also_with_proxy_to_pthread
  def test_preload_file_with_manual_data_download(self):
    create_file('file.txt', 'Hello!')

    self.compile_btest('browser/test_manual_download_data.c', ['-sEXIT_RUNTIME', '-o', 'out.js', '--preload-file', 'file.txt@/file.txt'])
    copy_asset('browser/test_manual_download_data.html')

    # Move .data file out of server root to ensure that getPreloadedPackage is actually used
    os.mkdir('test')
    shutil.move('out.js', 'test/test_manual_download_data.js')
    shutil.move('out.data', 'test/test_manual_download_data.data')

    self.run_browser('test_manual_download_data.html', '/report_result?exit:0')

  # Tests that if the output files have single or double quotes in them, that it will be handled by
  # correctly escaping the names.
  def test_output_file_escaping(self):
    self.set_setting('EXIT_RUNTIME')
    tricky_part = '\'' if WINDOWS else '\' and \"' # On Windows, files/directories may not contain a double quote character. On non-Windowses they can, so test that.

    d = 'dir with ' + tricky_part
    abs_d = os.path.abspath(d)
    ensure_dir(abs_d)
    txt = 'file with ' + tricky_part + '.txt'
    create_file(os.path.join(d, txt), 'load me right before')

    src = os.path.join(d, 'file with ' + tricky_part + '.c')
    create_file(src, r'''
      #include <assert.h>
      #include <stdio.h>
      #include <string.h>
      #include <emscripten.h>
      int main() {
        FILE *f = fopen("%s", "r");
        char buf[100];
        fread(buf, 1, 20, f);
        buf[20] = 0;
        fclose(f);
        printf("|%%s|\n", buf);
        assert(strcmp("load me right before", buf) == 0);
        return 0;
      }
    ''' % (txt.replace('\'', '\\\'').replace('\"', '\\"')))

    data_file = os.path.join(abs_d, 'file with ' + tricky_part + '.data')
    data_js_file = os.path.join(abs_d, 'file with ' + tricky_part + '.js')
    abs_txt = os.path.join(abs_d, txt)
    self.run_process([FILE_PACKAGER, data_file, '--use-preload-cache', '--indexedDB-name=testdb', '--preload', abs_txt + '@' + txt, '--js-output=' + data_js_file])
    page_file = os.path.join(d, 'file with ' + tricky_part + '.html')
    abs_page_file = os.path.abspath(page_file)
    self.compile_btest(src, ['--pre-js', data_js_file, '-o', abs_page_file, '-sFORCE_FILESYSTEM'], reporting=Reporting.JS_ONLY)
    self.run_browser(page_file, '/report_result?exit:0')

  # Clear all IndexedDB databases. This gives us a fresh state for tests that
  # check caching.
  def clear_indexed_db(self):
    self.add_browser_reporting()
    create_file('clear_indexed_db.html', '''
      <script src="browser_reporting.js"></script>
      <script>
        // Clear the cache, so that the next test starts from a clean slate.
        if (indexedDB.databases) {
          // If the tested browser supports IndexedDB 3.0 API, then enumerate all
          // available databases and delete them.
          indexedDB.databases().then(dbs => {
            Promise.all(dbs.map(db => {
              return indexedDB.deleteDatabase(db.name);
            })).then(() => {
              reportResultToServer("clear");
            });
          });
        } else {
          // Testing an old browser that does not support indexedDB.databases():
          // Delete the fixed database EM_PRELOAD_CACHE (this is hardcoded in
          // file packager)
          indexedDB.deleteDatabase('EM_PRELOAD_CACHE').onsuccess = () => {
            reportResultToServer("clear");
          };
        }
      </script>
    ''')
    self.run_browser('clear_indexed_db.html', '/report_result?clear')

  @parameterized({
    '0': (0,),
    '1mb': (1 * 1024 * 1024,),
    '100mb': (100 * 1024 * 1024,),
    '150mb': (150 * 1024 * 1024,),
  })
  def test_preload_caching(self, extra_size):
    self.clear_indexed_db()
    self.set_setting('EXIT_RUNTIME')
    create_file('main.c', r'''
      #include <assert.h>
      #include <stdio.h>
      #include <string.h>
      #include <emscripten.h>

      extern int checkPreloadResults();

      int main(int argc, char** argv) {
        FILE *f = fopen("%s", "r");
        char buf[100];
        fread(buf, 1, 20, f);
        buf[20] = 0;
        fclose(f);
        printf("|%%s|\n", buf);

        assert(strcmp("load me right before", buf) == 0);
        return checkPreloadResults();
      }
    ''' % 'somefile.txt')

    create_file('test.js', '''
      addToLibrary({
        checkPreloadResults: function() {
          var cached = 0;
          var packages = Object.keys(Module['preloadResults']);
          packages.forEach(function(package) {
            var fromCache = Module['preloadResults'][package]['fromCache'];
            if (fromCache) {
              cached++;
            }
          });
          return cached;
        }
      });
    ''')

    # test caching of various sizes, including sizes higher than 128MB which is
    # chrome's limit on IndexedDB item sizes, see
    # https://cs.chromium.org/chromium/src/content/renderer/indexed_db/webidbdatabase_impl.cc?type=cs&q=%22The+serialized+value+is+too+large%22&sq=package:chromium&g=0&l=177
    # https://cs.chromium.org/chromium/src/out/Debug/gen/third_party/blink/public/mojom/indexeddb/indexeddb.mojom.h?type=cs&sq=package:chromium&g=0&l=60
    if is_chrome() and extra_size >= 100 * 1024 * 1024:
      self.skipTest('chrome bug')
    create_file('somefile.txt', '''load me right before running the code please''' + ('_' * extra_size))
    print('size:', os.path.getsize('somefile.txt'))
    args = ['--use-preload-cache', '--js-library', 'test.js', '--preload-file', 'somefile.txt', '-o', 'page.html', '-sALLOW_MEMORY_GROWTH']
    self.compile_btest('main.c', args, reporting=Reporting.JS_ONLY)
    self.run_browser('page.html', '/report_result?exit:0')
    self.run_browser('page.html', '/report_result?exit:1')

    # test with ENVIRONMENT=web, to check for problems with node.js support
    # (see #23059)
    self.clear_indexed_db()
    self.compile_btest('main.c', args + ['-sENVIRONMENT=web'], reporting=Reporting.JS_ONLY)
    self.run_browser('page.html', '/report_result?exit:0')

  def test_preload_caching_indexeddb_name(self):
    self.set_setting('EXIT_RUNTIME')
    create_file('somefile.txt', 'load me right before running the code please')

    def make_main(path):
      print(path)
      create_file('main.c', r'''
        #include <assert.h>
        #include <stdio.h>
        #include <string.h>
        #include <emscripten.h>

        extern int checkPreloadResults();

        int main(int argc, char** argv) {
          FILE *f = fopen("%s", "r");
          char buf[100];
          fread(buf, 1, 20, f);
          buf[20] = 0;
          fclose(f);
          printf("|%%s|\n", buf);

          int result = 0;

          assert(strcmp("load me right before", buf) == 0);
          int num_cached = checkPreloadResults();
          printf("got %%d preloadResults from cache\n", num_cached);
          return num_cached;
        }
      ''' % path)

    create_file('test.js', '''
      addToLibrary({
        checkPreloadResults: () => {
          var cached = 0;
          for (var result of Object.values(Module['preloadResults'])) {
            if (result['fromCache']) {
              cached++;
            }
          }
          return cached;
        }
      });
    ''')

    make_main('somefile.txt')
    self.run_process([FILE_PACKAGER, 'somefile.data', '--use-preload-cache', '--indexedDB-name=testdb', '--preload', 'somefile.txt', '--js-output=' + 'somefile.js'])
    self.compile_btest('main.c', ['--js-library', 'test.js', '--pre-js', 'somefile.js', '-o', 'page.html', '-sFORCE_FILESYSTEM'], reporting=Reporting.JS_ONLY)
    self.run_browser('page.html', '/report_result?exit:0')
    print("Re-running ..")
    self.run_browser('page.html', '/report_result?exit:1')

  def test_multifile(self):
    # a few files inside a directory
    ensure_dir('subdirr/moar')
    create_file('subdirr/data1.txt', '1214141516171819')
    create_file('subdirr/moar/data2.txt', '3.14159265358979')
    create_file('main.c', r'''
      #include <assert.h>
      #include <stdio.h>
      #include <string.h>
      #include <emscripten.h>
      int main() {
        char buf[17];

        FILE *f = fopen("subdirr/data1.txt", "r");
        fread(buf, 1, 16, f);
        buf[16] = 0;
        fclose(f);
        printf("|%s|\n", buf);
        assert(strcmp("1214141516171819", buf) == 0);

        FILE *f2 = fopen("subdirr/moar/data2.txt", "r");
        fread(buf, 1, 16, f2);
        buf[16] = 0;
        fclose(f2);
        printf("|%s|\n", buf);
        assert(strcmp("3.14159265358979", buf) == 0);

        return 0;
      }
    ''')

    # by individual files
    self.btest_exit('main.c', cflags=['--preload-file', 'subdirr/data1.txt', '--preload-file', 'subdirr/moar/data2.txt'])

    # by directory, and remove files to make sure
    self.set_setting('EXIT_RUNTIME')
    self.compile_btest('main.c', ['--preload-file', 'subdirr', '-o', 'page.html'], reporting=Reporting.JS_ONLY)
    shutil.rmtree('subdirr')
    self.run_browser('page.html', '/report_result?exit:0')

  def test_custom_file_package_url(self):
    # a few files inside a directory
    ensure_dir('subdirr')
    ensure_dir('cdn')
    create_file(Path('subdirr/data1.txt'), '1214141516171819')
    # change the file package base dir to look in a "cdn". note that normally
    # you would add this in your own custom html file etc., and not by
    # modifying the existing shell in this manner
    default_shell = read_file(path_from_root('html/shell.html'))
    create_file('shell.html', default_shell.replace('var Module = {', '''
    var Module = {
      locateFile: function(path, prefix) {
        if (path.endsWith(".wasm")) {
           return prefix + path;
        } else {
           return "cdn/" + path;
        }
      },
    '''))
    create_file('main.c', r'''
      #include <assert.h>
      #include <stdio.h>
      #include <string.h>
      #include <emscripten.h>
      int main() {
        char buf[17];

        FILE *f = fopen("subdirr/data1.txt", "r");
        fread(buf, 1, 16, f);
        buf[16] = 0;
        fclose(f);
        printf("|%s|\n", buf);
        assert(strcmp("1214141516171819", buf) == 0);

        return 0;
      }
    ''')

    self.set_setting('EXIT_RUNTIME')
    self.compile_btest('main.c', ['--shell-file', 'shell.html', '--preload-file', 'subdirr/data1.txt', '-o', 'test.html'], reporting=Reporting.JS_ONLY)
    shutil.move('test.data', Path('cdn/test.data'))
    self.run_browser('test.html', '/report_result?exit:0')

  def test_missing_data_throws_error(self):
    create_file('data.txt', 'data')
    create_file('main.c', r'''
      #include <stdio.h>
      #include <string.h>
      #include <emscripten.h>
      int main() {
        // This code should never be executed in terms of missing required dependency file.
        return 0;
      }
    ''')

    def setup(assetLocalization):
      create_file('on_window_error_shell.html', r'''
      <html>
        <body>
          <center><canvas id='canvas' width='256' height='256'></canvas></center>
          <hr><div id='output'></div><hr>
          <script type='text/javascript'>
            const errorHandler = async (event) => {
              if (globalThis.disableErrorReporting) return;
              event.stopImmediatePropagation();
              const error = String(event instanceof ErrorEvent ? event.message : (event.reason || event));
              globalThis.disableErrorReporting = true;
              window.onerror = null;
              var result = error.includes("test.data") ? 1 : 0;
              await fetch('/report_result?' + result);
              window.close();
            }
            window.addEventListener('error', errorHandler);
            window.addEventListener('unhandledrejection', errorHandler);
            const outputElem = document.getElementById('output');
            var Module = {
              locateFile: (path, prefix) => {
                if (path.endsWith('.wasm')) {
                  return prefix + path;
                } else {
                  return "''' + assetLocalization + r'''" + path;
                }
              },
              print: () => {
                outputElem.innerHTML += text.replace('\n', '<br>', 'g') + '<br>';
              },
              canvas: document.getElementById('canvas')
            };
          </script>
          {{{ SCRIPT }}}
        </body>
      </html>''')

    # test test missing file should run xhr.onload with status different than 200, 304 or 206
    setup("")
    self.compile_btest('main.c', ['--shell-file', 'on_window_error_shell.html', '--preload-file', 'data.txt', '-o', 'test.html'])
    shutil.move('test.data', 'missing.data')
    self.run_browser('test.html', '/report_result?1')

    # test unknown protocol should go through xhr.onerror
    setup("unknown_protocol://")
    self.compile_btest('main.c', ['--shell-file', 'on_window_error_shell.html', '--preload-file', 'data.txt', '-o', 'test.html'])
    self.run_browser('test.html', '/report_result?1')

    # test wrong protocol and port
    setup("https://localhost:8800/")
    self.compile_btest('main.c', ['--shell-file', 'on_window_error_shell.html', '--preload-file', 'data.txt', '-o', 'test.html'])
    self.run_browser('test.html', '/report_result?1')

    # TODO: CORS, test using a full url for locateFile
    # create_file('shell.html', read_file(path_from_root('html/shell.html')).replace('var Module = {', 'var Module = { locateFile: function (path) {return "http:/localhost:8888/cdn/" + path;}, '))
    # test()

  @also_with_wasmfs
  def test_fs_dev_random(self):
    self.btest_exit('fs/test_fs_dev_random.c')

  def test_sdl_swsurface(self):
    self.btest_exit('test_sdl_swsurface.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_surface_lock_opts(self):
    # Test Emscripten-specific extensions to optimize SDL_LockSurface and SDL_UnlockSurface.
    self.reftest('hello_world_sdl.c', 'browser/htmltest.png', cflags=['-DTEST_SDL_LOCK_OPTS', '-lSDL', '-lGL'])

  @also_with_wasmfs
  def test_sdl_image(self):
    # load an image file, get pixel data. Also O2 coverage for --preload-file
    copy_asset('browser/screenshot.jpg')
    src = test_file('browser/test_sdl_image.c')
    for dest, dirname, basename in [('screenshot.jpg', '/', 'screenshot.jpg'),
                                    ('screenshot.jpg@/assets/screenshot.jpg', '/assets', 'screenshot.jpg')]:
      self.btest_exit(src, cflags=[
        '-O2', '-lSDL', '-lGL',
        '--preload-file', dest, '-DSCREENSHOT_DIRNAME="' + dirname + '"', '-DSCREENSHOT_BASENAME="' + basename + '"', '--use-preload-plugins',
      ])

  @also_with_wasmfs
  def test_sdl_image_jpeg(self):
    copy_asset('browser/screenshot.jpg')
    self.btest_exit('test_sdl_image.c', cflags=[
      '--preload-file', 'screenshot.jpg',
      '-DSCREENSHOT_DIRNAME="/"', '-DSCREENSHOT_BASENAME="screenshot.jpg"', '--use-preload-plugins',
      '-lSDL', '-lGL',
    ])

  def test_sdl_image_webp(self):
    copy_asset('browser/screenshot.webp')
    self.btest_exit('test_sdl_image.c', cflags=[
      '--preload-file', 'screenshot.webp',
      '-DSCREENSHOT_DIRNAME="/"', '-DSCREENSHOT_BASENAME="screenshot.webp"', '--use-preload-plugins',
      '-lSDL', '-lGL',
    ])

  @also_with_wasmfs
  def test_sdl_image_prepare(self):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.reftest('test_sdl_image_prepare.c', 'screenshot.jpg', cflags=['--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

  @parameterized({
    '': ([],),
    # add testing for closure on preloaded files + ENVIRONMENT=web (we must not
    # emit any node.js code here, see
    # https://github.com/emscripten-core/emscripten/issues/14486
    'closure_webonly': (['--closure', '1', '-sENVIRONMENT=web'],),
  })
  def test_sdl_image_prepare_data(self, args):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.reftest('test_sdl_image_prepare_data.c', 'screenshot.jpg', cflags=['--preload-file', 'screenshot.not', '-lSDL', '-lGL'] + args)

  def test_sdl_image_must_prepare(self):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.jpg')
    self.reftest('test_sdl_image_must_prepare.c', 'screenshot.jpg', cflags=['--preload-file', 'screenshot.jpg', '-lSDL', '-lGL'])

  def test_sdl_stb_image(self):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.reftest('test_sdl_stb_image.c', 'screenshot.jpg', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

  def test_sdl_stb_image_bpp(self):
    # load grayscale image without alpha
    copy_asset('browser/test_sdl-stb-bpp1.png', 'screenshot.not')
    self.reftest('test_sdl_stb_image.c', 'test_sdl-stb-bpp1.png', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

    # load grayscale image with alpha
    self.clear()
    copy_asset('browser/test_sdl-stb-bpp2.png', 'screenshot.not')
    self.reftest('test_sdl_stb_image.c', 'test_sdl-stb-bpp2.png', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

    # load RGB image
    self.clear()
    copy_asset('browser/test_sdl-stb-bpp3.png', 'screenshot.not')
    self.reftest('test_sdl_stb_image.c', 'test_sdl-stb-bpp3.png', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

    # load RGBA image
    self.clear()
    copy_asset('browser/test_sdl-stb-bpp4.png', 'screenshot.not')
    self.reftest('test_sdl_stb_image.c', 'test_sdl-stb-bpp4.png', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

  def test_sdl_stb_image_data(self):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.reftest('test_sdl_stb_image_data.c', 'screenshot.jpg', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL'])

  def test_sdl_stb_image_cleanup(self):
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.btest_exit('test_sdl_stb_image_cleanup.c', cflags=['-sSTB_IMAGE', '--preload-file', 'screenshot.not', '-lSDL', '-lGL', '--memoryprofiler'])

  @parameterized({
    '': ([],),
    'safe_heap': (['-sSAFE_HEAP'],),
    'safe_heap_O2': (['-sSAFE_HEAP', '-O2'],),
  })
  def test_sdl_canvas(self, args):
    self.btest_exit('test_sdl_canvas.c', cflags=['-sSTRICT_JS', '-sLEGACY_GL_EMULATION', '-lSDL', '-lGL'] + args)

  def test_sdl_canvas_alpha(self):
    # N.B. On Linux with Intel integrated graphics cards, this test needs Firefox 49 or newer.
    # See https://github.com/emscripten-core/emscripten/issues/4069.
    create_file('flag_0.js', "Module['arguments'] = ['-0'];")

    self.reftest('test_sdl_canvas_alpha.c', cflags=['-lSDL', '-lGL'], reference_slack=12)
    self.reftest('test_sdl_canvas_alpha.c', 'test_sdl_canvas_alpha_flag_0.png', cflags=['--pre-js', 'flag_0.js', '-lSDL', '-lGL'], reference_slack=12)

  @parameterized({
    '': ([],),
    'eventhandler': (['-DTEST_EMSCRIPTEN_SDL_SETEVENTHANDLER'],),
  })
  @parameterized({
    '': ([],),
    'asyncify': (['-DTEST_SLEEP', '-sASSERTIONS', '-sSAFE_HEAP', '-sASYNCIFY'],),
  })
  @parameterized({
    '': (False,),
    'delay': (True,),
  })
  def test_sdl_key(self, defines, async_, delay):
    if delay:
      settimeout_start = 'setTimeout(function() {'
      settimeout_end = '}, 1);'
    else:
      settimeout_start = ''
      settimeout_end = ''
    create_file('pre.js', '''
      function keydown(c) {
       %s
        simulateKeyDown(c);
       %s
      }

      function keyup(c) {
       %s
        simulateKeyUp(c);
       %s
      }
    ''' % (settimeout_start, settimeout_end, settimeout_start, settimeout_end))
    self.btest_exit('test_sdl_key.c', 223092870, cflags=defines + async_ + ['--pre-js', test_file('browser/fake_events.js'), '--pre-js=pre.js', '-lSDL', '-lGL'])

  def test_canvas_focus(self):
    self.btest_exit('test_canvas_focus.c', cflags=['--pre-js', test_file('browser/fake_events.js')])

  def test_sdl_text(self):
    create_file('pre.js', '''
      Module.postRun = () => {
        function doOne() {
          Module._one();
          setTimeout(doOne, 1000/60);
        }
        setTimeout(doOne, 1000/60);
      }
    ''')

    self.btest_exit('test_sdl_text.c', cflags=['--pre-js', 'pre.js', '--pre-js', test_file('browser/fake_events.js'), '-lSDL', '-lGL'])

  def test_sdl_mouse(self):
    self.btest_exit('test_sdl_mouse.c', cflags=['-O2', '--minify=0', '--pre-js', test_file('browser/fake_events.js'), '-lSDL', '-lGL'])

  def test_sdl_mouse_offsets(self):
    create_file('page.html', '''
      <html>
        <head>
          <style type="text/css">
            html, body { margin: 0; padding: 0; }
            #container {
              position: absolute;
              left: 5px; right: 0;
              top: 5px; bottom: 0;
            }
            #canvas {
              position: absolute;
              left: 0; width: 600px;
              top: 0; height: 450px;
            }
            textarea {
              margin-top: 500px;
              margin-left: 5px;
              width: 600px;
            }
          </style>
        </head>
        <body>
          <div id="container">
            <canvas id="canvas"></canvas>
          </div>
          <textarea id="output" rows="8"></textarea>
          <script type="text/javascript">
            var Module = {
              canvas: document.getElementById('canvas'),
              print: (function() {
                var element = document.getElementById('output');
                element.value = ''; // clear browser cache
                return function(text) {
                  if (arguments.length > 1) text = Array.prototype.slice.call(arguments).join(' ');
                  element.value += text + "\\n";
                  element.scrollTop = element.scrollHeight; // focus on bottom
                };
              })()
            };
          </script>
          <script type="text/javascript" src="sdl_mouse.js"></script>
        </body>
      </html>
    ''')

    self.compile_btest('browser/test_sdl_mouse.c', ['-DTEST_SDL_MOUSE_OFFSETS', '-O2', '--minify=0', '-o', 'sdl_mouse.js', '--pre-js', test_file('browser/fake_events.js'), '-lSDL', '-lGL', '-sEXIT_RUNTIME'])
    self.run_browser('page.html', '', '/report_result?exit:0')

  def test_glut_touchevents(self):
    self.btest_exit('glut_touchevents.c', cflags=['-lglut', '-sSTRICT_JS'])

  def test_glut_wheelevents(self):
    self.btest_exit('glut_wheelevents.c', cflags=['-lglut'])

  @requires_graphics_hardware
  def test_glut_glutget_no_antialias(self):
    self.btest_exit('glut_glutget.c', cflags=['-lglut', '-lGL'])
    self.btest_exit('glut_glutget.c', cflags=['-lglut', '-lGL', '-DDEPTH_ACTIVATED', '-DSTENCIL_ACTIVATED', '-DALPHA_ACTIVATED'])

  # This test supersedes the one above, but it's skipped in the CI because anti-aliasing is not well supported by the Mesa software renderer.
  @requires_graphics_hardware
  def test_glut_glutget(self):
    self.btest_exit('glut_glutget.c', cflags=['-lglut', '-lGL'])
    self.btest_exit('glut_glutget.c', cflags=['-lglut', '-lGL', '-DAA_ACTIVATED', '-DDEPTH_ACTIVATED', '-DSTENCIL_ACTIVATED', '-DALPHA_ACTIVATED'])

  def test_glut_resize(self):
    self.btest_exit('test_glut_resize.c')

  def test_sdl_joystick_1(self):
    # Generates events corresponding to the Working Draft of the HTML5 Gamepad API.
    # http://www.w3.org/TR/2012/WD-gamepad-20120529/#gamepad-interface
    create_file('pre.js', '''
      var gamepads = [];
      // Spoof this function.
      navigator['getGamepads'] = () => gamepads;
      window['addNewGamepad'] = (id, numAxes, numButtons) => {
        var index = gamepads.length;
        gamepads.push({
          axes: new Array(numAxes),
          buttons: new Array(numButtons),
          id: id,
          index: index
        });
        var i;
        for (i = 0; i < numAxes; i++) gamepads[index].axes[i] = 0;
        for (i = 0; i < numButtons; i++) gamepads[index].buttons[i] = 0;
      };
      window['simulateGamepadButtonDown'] = (index, button) => {
        gamepads[index].buttons[button] = 1;
      };
      window['simulateGamepadButtonUp'] = (index, button) => {
        gamepads[index].buttons[button] = 0;
      };
      window['simulateAxisMotion'] = (index, axis, value) => {
        gamepads[index].axes[axis] = value;
      };
    ''')

    self.btest_exit('test_sdl_joystick.c', cflags=['-O2', '--minify=0', '-o', 'page.html', '--pre-js', 'pre.js', '-lSDL', '-lGL'])

  def test_sdl_joystick_2(self):
    # Generates events corresponding to the Editor's Draft of the HTML5 Gamepad API.
    # https://dvcs.w3.org/hg/gamepad/raw-file/default/gamepad.html#idl-def-Gamepad
    create_file('pre.js', '''
      var gamepads = [];
      // Spoof this function.
      navigator['getGamepads'] = () => gamepads;
      window['addNewGamepad'] = (id, numAxes, numButtons) => {
        var index = gamepads.length;
        gamepads.push({
          axes: new Array(numAxes),
          buttons: new Array(numButtons),
          id: id,
          index: index
        });
        var i;
        for (i = 0; i < numAxes; i++) gamepads[index].axes[i] = 0;
        // Buttons are objects
        for (i = 0; i < numButtons; i++) gamepads[index].buttons[i] = { pressed: false, value: 0 };
      };
      // FF mutates the original objects.
      window['simulateGamepadButtonDown'] = (index, button) => {
        gamepads[index].buttons[button].pressed = true;
        gamepads[index].buttons[button].value = 1;
      };
      window['simulateGamepadButtonUp'] = (index, button) => {
        gamepads[index].buttons[button].pressed = false;
        gamepads[index].buttons[button].value = 0;
      };
      window['simulateAxisMotion'] = (index, axis, value) => {
        gamepads[index].axes[axis] = value;
      };
    ''')

    self.btest_exit('test_sdl_joystick.c', cflags=['-O2', '--minify=0', '--pre-js', 'pre.js', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_glfw_joystick(self):
    # Generates events corresponding to the Editor's Draft of the HTML5 Gamepad API.
    # https://dvcs.w3.org/hg/gamepad/raw-file/default/gamepad.html#idl-def-Gamepad
    create_file('pre.js', '''
      var gamepads = [];
      // Spoof this function.
      navigator['getGamepads'] = () => gamepads;
      window['addNewGamepad'] = (id, numAxes, numButtons) => {
        var index = gamepads.length;
        var gamepad = {
          axes: new Array(numAxes),
          buttons: new Array(numButtons),
          id: id,
          index: index
        };
        gamepads.push(gamepad)
        var i;
        for (i = 0; i < numAxes; i++) gamepads[index].axes[i] = 0;
        // Buttons are objects
        for (i = 0; i < numButtons; i++) gamepads[index].buttons[i] = { pressed: false, value: 0 };

        // Dispatch event (required for glfw joystick; note not used in SDL test)
        var event = new Event('gamepadconnected');
        event.gamepad = gamepad;
        window.dispatchEvent(event);
      };
      // FF mutates the original objects.
      window['simulateGamepadButtonDown'] = (index, button) => {
        gamepads[index].buttons[button].pressed = true;
        gamepads[index].buttons[button].value = 1;
      };
      window['simulateGamepadButtonUp'] = (index, button) => {
        gamepads[index].buttons[button].pressed = false;
        gamepads[index].buttons[button].value = 0;
      };
      window['simulateAxisMotion'] = (index, axis, value) => {
        gamepads[index].axes[axis] = value;
      };
    ''')

    self.btest_exit('test_glfw_joystick.c', cflags=['-O2', '--minify=0', '-o', 'page.html', '--pre-js', 'pre.js', '-lGL', '-lglfw3', '-sUSE_GLFW=3'])

  @requires_graphics_hardware
  def test_webgl_context_attributes(self):
    # Javascript code to check the attributes support we want to test in the WebGL implementation
    # (request the attribute, create a context and check its value afterwards in the context attributes).
    # Tests will succeed when an attribute is not supported.
    create_file('check_webgl_attributes_support.js', '''
      addToLibrary({
        webglAntialiasSupported: function() {
          canvas = document.createElement('canvas');
          context = canvas.getContext('experimental-webgl', {antialias: true});
          attributes = context.getContextAttributes();
          return attributes.antialias;
        },
        webglDepthSupported: function() {
          canvas = document.createElement('canvas');
          context = canvas.getContext('experimental-webgl', {depth: true});
          attributes = context.getContextAttributes();
          return attributes.depth;
        },
        webglStencilSupported: function() {
          canvas = document.createElement('canvas');
          context = canvas.getContext('experimental-webgl', {stencil: true});
          attributes = context.getContextAttributes();
          return attributes.stencil;
        },
        webglAlphaSupported: function() {
          canvas = document.createElement('canvas');
          context = canvas.getContext('experimental-webgl', {alpha: true});
          attributes = context.getContextAttributes();
          return attributes.alpha;
        }
      });
    ''')

    # Copy common code file to temporary directory
    filepath = test_file('browser/test_webgl_context_attributes_common.c')
    temp_filepath = os.path.basename(filepath)
    shutil.copy(filepath, temp_filepath)

    # testAntiAliasing uses a window-sized buffer on the stack
    self.set_setting('STACK_SIZE', '1MB')

    # perform tests with attributes activated
    self.btest_exit('test_webgl_context_attributes_glut.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-DAA_ACTIVATED', '-DDEPTH_ACTIVATED', '-DSTENCIL_ACTIVATED', '-DALPHA_ACTIVATED', '-lGL', '-lglut', '-lGLEW'])
    self.btest_exit('test_webgl_context_attributes_sdl.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-DAA_ACTIVATED', '-DDEPTH_ACTIVATED', '-DSTENCIL_ACTIVATED', '-DALPHA_ACTIVATED', '-lGL', '-lSDL', '-lGLEW'])
    if not self.is_wasm64():
      self.btest_exit('test_webgl_context_attributes_sdl2.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-DAA_ACTIVATED', '-DDEPTH_ACTIVATED', '-DSTENCIL_ACTIVATED', '-DALPHA_ACTIVATED', '-lGL', '-sUSE_SDL=2', '-lGLEW'])
    self.btest_exit('test_webgl_context_attributes_glfw.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-DAA_ACTIVATED', '-DDEPTH_ACTIVATED', '-DSTENCIL_ACTIVATED', '-DALPHA_ACTIVATED', '-lGL', '-lglfw', '-lGLEW'])

    # perform tests with attributes deactivated
    self.btest_exit('test_webgl_context_attributes_glut.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-lGL', '-lglut', '-lGLEW'])
    self.btest_exit('test_webgl_context_attributes_sdl.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-lGL', '-lSDL', '-lGLEW'])
    self.btest_exit('test_webgl_context_attributes_glfw.c', cflags=['--js-library', 'check_webgl_attributes_support.js', '-lGL', '-lglfw', '-lGLEW'])

  @requires_graphics_hardware
  def test_webgl_no_double_error(self):
    self.btest_exit('webgl_error.c')

  @requires_graphics_hardware
  def test_webgl_parallel_shader_compile(self):
    self.btest_exit('webgl_parallel_shader_compile.cpp')

  @requires_webgl2
  def test_webgl_explicit_uniform_location(self):
    self.btest_exit('webgl_explicit_uniform_location.c', cflags=['-sGL_EXPLICIT_UNIFORM_LOCATION', '-sMIN_WEBGL_VERSION=2'])

  @requires_graphics_hardware
  def test_webgl_sampler_layout_binding(self):
    self.btest_exit('webgl_sampler_layout_binding.c', cflags=['-sGL_EXPLICIT_UNIFORM_BINDING'])

  @requires_webgl2
  def test_webgl2_ubo_layout_binding(self):
    self.btest_exit('webgl2_ubo_layout_binding.c', cflags=['-sGL_EXPLICIT_UNIFORM_BINDING', '-sMIN_WEBGL_VERSION=2'])

  # Test that -sGL_PREINITIALIZED_CONTEXT works and allows user to set Module['preinitializedWebGLContext'] to a preinitialized WebGL context.
  @requires_graphics_hardware
  def test_preinitialized_webgl_context(self):
    self.btest_exit('test_preinitialized_webgl_context.c', cflags=['-sGL_PREINITIALIZED_CONTEXT', '--shell-file', test_file('browser/test_preinitialized_webgl_context.html')])

  @parameterized({
    '': ([],),
    'threads': (['-pthread'],),
    'closure': (['-sENVIRONMENT=web', '-O2', '--closure=1'],),
  })
  def test_emscripten_get_now(self, args):
    self.btest_exit('test_emscripten_get_now.c', cflags=args)

  def test_write_file_in_environment_web(self):
    self.btest_exit('write_file.c', cflags=['-sENVIRONMENT=web', '-Os', '--closure=1'])

  def test_fflush(self):
    self.btest('test_fflush.cpp', '0', cflags=['-sEXIT_RUNTIME', '--shell-file', test_file('test_fflush.html')], reporting=Reporting.NONE)

  @parameterized({
    '': ([],),
    'extra': (['-DEXTRA_WORK'],),
    'autopersist': (['-DIDBFS_AUTO_PERSIST'],),
  })
  def test_fs_idbfs_sync(self, args):
    secret = str(time.time())
    self.btest_exit('fs/test_idbfs_sync.c', cflags=['-lidbfs.js', f'-DSECRET="{secret}"', '-lidbfs.js'] + args + ['-DFIRST'])
    print('done first half')
    self.btest_exit('fs/test_idbfs_sync.c', cflags=['-lidbfs.js', f'-DSECRET="{secret}"', '-lidbfs.js'] + args)

  @parameterized({
    'open': ('TEST_CASE_OPEN', 2),
    'close': ('TEST_CASE_CLOSE', 3),
    'symlink': ('TEST_CASE_SYMLINK', 3),
    'unlink': ('TEST_CASE_UNLINK', 3),
    'rename': ('TEST_CASE_RENAME', 3),
    'mkdir': ('TEST_CASE_MKDIR', 2),
  })
  def test_fs_idbfs_autopersist(self, test_case, phase_count):
    self.cflags += ['-lidbfs.js', f'-DTEST_CASE={test_case}']
    for phase in range(phase_count):
      self.btest_exit('fs/test_idbfs_autopersist.c', cflags=[f'-DTEST_PHASE={phase + 1}'])

  def test_fs_idbfs_fsync(self):
    # sync from persisted state into memory before main()
    self.set_setting('DEFAULT_LIBRARY_FUNCS_TO_INCLUDE', '$ccall,$addRunDependency')
    create_file('pre.js', '''
      Module.preRun = () => {
        addRunDependency('syncfs');

        FS.mkdir('/working1');
        FS.mount(IDBFS, {}, '/working1');
        FS.syncfs(true, function (err) {
          if (err) throw err;
          removeRunDependency('syncfs');
        });
      };
    ''')

    args = ['--pre-js', 'pre.js', '-lidbfs.js', '-sEXIT_RUNTIME', '-sASYNCIFY']
    secret = str(time.time())
    self.btest('fs/test_idbfs_fsync.c', '1', cflags=args + ['-DFIRST', f'-DSECRET="{secret}"', '-lidbfs.js'])
    self.btest('fs/test_idbfs_fsync.c', '1', cflags=args + [f'-DSECRET="{secret}"', '-lidbfs.js'])

  def test_fs_memfs_fsync(self):
    self.btest_exit('fs/test_memfs_fsync.c', cflags=['-sASYNCIFY', '-sEXIT_RUNTIME'])

  def test_fs_workerfs(self):
    create_file('pre.js', '''
      Module.preRun = () => {
        var blob = new Blob(['hello blob']);
        var file = new File(['hello file'], 'file.txt');
        FS.mkdir('/work');
        FS.mount(WORKERFS, {
          blobs: [{ name: 'blob.txt', data: blob }],
          files: [file],
        }, '/work');
      };
    ''')
    self.btest_exit('fs/test_workerfs.c', cflags=['-lworkerfs.js', '--pre-js', 'pre.js'], run_in_worker=True)

  def test_fs_workerfs_package(self):
    create_file('file1.txt', 'first')
    ensure_dir('sub')
    create_file('sub/file2.txt', 'second')
    self.run_process([FILE_PACKAGER, 'files.data', '--preload', 'file1.txt', 'sub/file2.txt', '--separate-metadata', '--js-output=files.js'])
    self.btest('fs/test_workerfs_package.c', '0', cflags=['-lworkerfs.js'], run_in_worker=True)

  def test_fs_lz4fs_package(self):
    # generate data
    ensure_dir('subdir')
    create_file('file1.txt', '0123456789' * (1024 * 128))
    create_file('subdir/file2.txt', '1234567890' * (1024 * 128))
    random_data = bytearray(random.randint(0, 255) for x in range(1024 * 128 * 10 + 1))
    random_data[17] = ord('X')
    create_file('file3.txt', random_data, binary=True)

    # compress in emcc, -sLZ4 tells it to tell the file packager
    print('emcc-normal')
    self.btest_exit('fs/test_lz4fs.c', 0, cflags=['-sLZ4', '--preload-file', 'file1.txt', '--preload-file', 'subdir/file2.txt', '--preload-file', 'file3.txt'])
    assert os.path.getsize('file1.txt') + os.path.getsize('subdir/file2.txt') + os.path.getsize('file3.txt') == 3 * 1024 * 128 * 10 + 1
    assert os.path.getsize('test.data') < (3 * 1024 * 128 * 10) / 2  # over half is gone
    print('    emcc-opts')
    self.btest_exit('fs/test_lz4fs.c', 0, cflags=['-sLZ4', '--preload-file', 'file1.txt', '--preload-file', 'subdir/file2.txt', '--preload-file', 'file3.txt', '-O2'])

    # compress in the file packager, on the server. the client receives compressed data and can just
    # use it. this is typical usage
    print('normal')
    out = subprocess.check_output([FILE_PACKAGER, 'files.data', '--preload', 'file1.txt', 'subdir/file2.txt', 'file3.txt', '--lz4'])
    create_file('files.js', out, binary=True)
    self.btest_exit('fs/test_lz4fs.c', 0, cflags=['--pre-js', 'files.js', '-sLZ4', '-sFORCE_FILESYSTEM'])
    print('    opts')
    self.btest_exit('fs/test_lz4fs.c', 0, cflags=['--pre-js', 'files.js', '-sLZ4', '-sFORCE_FILESYSTEM', '-O2'])
    print('    modularize')
    self.compile_btest('fs/test_lz4fs.c', ['--pre-js', 'files.js', '-sLZ4', '-sFORCE_FILESYSTEM', '-sMODULARIZE', '-sEXIT_RUNTIME'])
    create_file('a.html', '''
      <script src="a.out.js"></script>
      <script>
        Module()
      </script>
    ''')
    self.run_browser('a.html', '/report_result?exit:0')

    # load the data into LZ4FS manually at runtime. This means we compress on the client. This is
    # generally not recommended
    print('manual')
    subprocess.check_output([FILE_PACKAGER, 'files.data', '--preload', 'file1.txt', 'subdir/file2.txt', 'file3.txt', '--separate-metadata', '--js-output=files.js'])
    self.btest_exit('fs/test_lz4fs.c', 1, cflags=['-DLOAD_MANUALLY', '-sLZ4', '-sFORCE_FILESYSTEM'])
    print('    opts')
    self.btest_exit('fs/test_lz4fs.c', 1, cflags=['-DLOAD_MANUALLY', '-sLZ4', '-sFORCE_FILESYSTEM', '-O2'])
    print('    opts+closure')
    self.btest_exit('fs/test_lz4fs.c', 1, cflags=['-DLOAD_MANUALLY', '-sLZ4',
                                                  '-sFORCE_FILESYSTEM', '-O2',
                                                  '--closure=1', '-g1', '-Wno-closure'])

    # non-lz4 for comparison
    # try:
    #   os.mkdir('files')
    # except OSError:
    #   pass
    # shutil.copy('file1.txt', 'files/'))
    # shutil.copy('file2.txt', 'files/'))
    # shutil.copy('file3.txt', 'files/'))
    # out = subprocess.check_output([FILE_PACKAGER, 'files.data', '--preload', 'files/file1.txt', 'files/file2.txt', 'files/file3.txt'])
    # create_file('files.js', out, binary=True)

  def test_separate_metadata_later(self):
    # see issue #6654 - we need to handle separate-metadata both when we run before
    # the main program, and when we are run later

    create_file('data.dat', ' ')
    self.run_process([FILE_PACKAGER, 'more.data', '--preload', 'data.dat', '--separate-metadata', '--js-output=more.js'])
    self.btest(Path('browser/separate_metadata_later.cpp'), '1', cflags=['-sFORCE_FILESYSTEM'])

  @requires_safari_version(260001, 'TODO: Fails with "Assertion failed: false"') # Fails in Safari 18.5 (20621.2.5.11.8) with Intel x64 CPU only. Passes on Safari 18.5 (20621.2.5.11.8) with ARM M1, Safari 17.6 (17618.3.11.11.7, 17618)/x64 and Safari 26.0.1 (21622.1.22.11.15)/M4
  def test_idbstore(self):
    secret = str(time.time())
    for stage in (0, 1, 2, 3, 0, 1, 2, 0, 0, 1, 4, 2, 5, 0, 4, 6, 5):
      print(stage)
      self.btest_exit('test_idbstore.c',
                      cflags=['-lidbstore.js', f'-DSTAGE={stage}', f'-DSECRET="{secret}"'],
                      output_basename=f'idbstore_{stage}')

  @parameterized({
    'asyncify': (['-sASYNCIFY'],),
    'jspi': (['-sJSPI'],),
  })
  def test_idbstore_sync(self, args):
    if is_jspi(args):
      self.require_jspi()
    secret = str(time.time())
    self.btest('test_idbstore_sync.c', '8', cflags=['-sSTRICT', '-lidbstore.js', f'-DSECRET="{secret}"', '-O3', '--closure=1'] + args)

  def test_force_exit(self):
    self.btest_exit('test_force_exit.c')

  def test_sdl_pumpevents(self):
    # key events should be detected using SDL_PumpEvents
    self.btest_exit('test_sdl_pumpevents.c', cflags=['--pre-js', test_file('browser/fake_events.js'), '-lSDL', '-lGL'])

  def test_sdl_canvas_size(self):
    self.btest_exit('test_sdl_canvas_size.c',
                    cflags=['-O2', '--minify=0', '--shell-file',
                               test_file('browser/test_sdl_canvas_size.html'), '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_gl_extensions(self):
    self.btest_exit('test_sdl_gl_extensions.c', cflags=['-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_gl_read(self):
    # SDL, OpenGL, readPixels
    self.btest_exit('test_sdl_gl_read.c', cflags=['-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_gl_mapbuffers(self):
    self.btest_exit('test_sdl_gl_mapbuffers.c', cflags=['-sFULL_ES3', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_ogl(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_ogl.c', 'screenshot-gray-purple.png', reference_slack=1,
                 cflags=['-O2', '--minify=0', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_ogl_regal(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_ogl.c', 'screenshot-gray-purple.png', reference_slack=1,
                 cflags=['-O2', '--minify=0', '--preload-file', 'screenshot.png', '-sUSE_REGAL', '-DUSE_REGAL', '--use-preload-plugins', '-lSDL', '-lGL', '-lc++', '-lc++abi'])

  @requires_graphics_hardware
  def test_sdl_ogl_defaultmatrixmode(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_ogl_defaultMatrixMode.c', 'screenshot-gray-purple.png', reference_slack=1,
                 cflags=['--minify=0', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_ogl_p(self):
    # Immediate mode with pointers
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_ogl_p.c', 'screenshot-gray.png', reference_slack=1,
                 cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_ogl_proc_alias(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_ogl_proc_alias.c', 'screenshot-gray-purple.png', reference_slack=1,
                 cflags=['-O2', '-g2', '-sINLINING_LIMIT', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '-sGL_ENABLE_GET_PROC_ADDRESS', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_fog_simple(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_fog_simple.c', 'screenshot-fog-simple.png',
                 cflags=['-O2', '--minify=0', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_fog_negative(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_fog_negative.c', 'screenshot-fog-negative.png',
                 cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_fog_density(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_fog_density.c', 'screenshot-fog-density.png',
                 cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_fog_exp2(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_fog_exp2.c', 'screenshot-fog-exp2.png',
                 cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_sdl_fog_linear(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_fog_linear.c', 'screenshot-fog-linear.png', reference_slack=1,
                 cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins', '-lSDL', '-lGL'])

  @requires_graphics_hardware
  @parameterized({
    '': (['-lglfw'],),
    's_flag': (['-sUSE_GLFW=2'],),
    'both_flags': (['-sUSE_GLFW=2', '-lglfw'],),
  })
  def test_glfw(self, args):
    self.btest_exit('test_glfw.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-sGL_ENABLE_GET_PROC_ADDRESS'] + args)

  @parameterized({
    '': ([],),
    's_flag': (['-sUSE_GLFW=2'],),
  })
  def test_glfw_minimal(self, args):
    self.btest_exit('test_glfw_minimal.c', cflags=['-lglfw', '-lGL'] + args)

  def test_glfw_time(self):
    self.btest_exit('test_glfw_time.c', cflags=['-sUSE_GLFW=3', '-lglfw', '-lGL'])

  @parameterized({
    '': ([],),
    'proxy_to_pthread': (['-pthread', '-sPROXY_TO_PTHREAD', '-sOFFSCREEN_FRAMEBUFFER'],),
  })
  @requires_graphics_hardware
  def test_egl(self, args):
    self.btest_exit('test_egl.c', cflags=['-O2', '-lEGL', '-lGL', '-sGL_ENABLE_GET_PROC_ADDRESS'] + args)

  @also_with_proxy_to_pthread
  def test_egl_width_height(self):
    self.btest_exit('test_egl_width_height.c', cflags=['-O2', '-lEGL', '-lGL'])

  @requires_graphics_hardware
  def test_egl_createcontext_error(self):
    self.btest_exit('test_egl_createcontext_error.c', cflags=['-lEGL', '-lGL'])

  @parameterized({
    '': ([False],),
    'preload': ([True],),
  })
  def test_hello_world_worker(self, file_data):
    # Test running in a web worker
    create_file('file.dat', 'data for worker')
    create_file('main.html', '''
      <html>
      <body>
        Worker Test
        <script>
          var worker = new Worker('worker.js');
          worker.onmessage = async (event) => {
            await fetch('http://localhost:%s/report_result?' + event.data);
            window.close();
          };
        </script>
      </body>
      </html>
    ''' % self.PORT)

    cmd = [EMCC, test_file('hello_world_worker.c'), '-o', 'worker.js'] + self.get_cflags()
    if file_data:
      cmd += ['--preload-file', 'file.dat']
    self.run_process(cmd)
    self.assertExists('worker.js')
    self.run_browser('main.html', '/report_result?hello from worker, and :' + ('data for w' if file_data else '') + ':')

  @no_wasmfs('https://github.com/emscripten-core/emscripten/issues/19608')
  def test_mmap_lazyfile(self):
    create_file('lazydata.dat', 'hello world')
    create_file('pre.js', '''
      Module["preInit"] = () => {
        FS.createLazyFile('/', "lazy.txt", "lazydata.dat", true, false);
      }
    ''')
    self.btest_exit('test_mmap_lazyfile.c', cflags=['--pre-js=pre.js'], run_in_worker=True)

  @no_wasmfs('https://github.com/emscripten-core/emscripten/issues/19608')
  @no_firefox('keeps sending OPTIONS requests, and eventually errors')
  def test_chunked_synchronous_xhr(self):
    main = 'chunked_sync_xhr.html'
    worker_filename = "download_and_checksum_worker.js"

    create_file(main, r"""
      <!doctype html>
      <html>
      <head><meta charset="utf-8"><title>Chunked XHR</title></head>
      <body>
        Chunked XHR Web Worker Test
        <script>
          var worker = new Worker("%s");
          var buffer = [];
          worker.onmessage = async (event) => {
            if (event.data.channel === "stdout") {
              await fetch('http://localhost:%s/report_result?' + event.data.line);
              window.close();
            } else {
              if (event.data.trace) event.data.trace.split("\n").map(function(v) { console.error(v); });
              if (event.data.line) {
                console.error(event.data.line);
              } else {
                var v = event.data.char;
                if (v == 10) {
                  var line = buffer.splice(0);
                  console.error(line = line.map(function(charCode){return String.fromCharCode(charCode);}).join(''));
                } else {
                  buffer.push(v);
                }
              }
            }
          };
        </script>
      </body>
      </html>
    """ % (worker_filename, self.PORT))

    create_file('worker_prejs.js', r"""
      Module.arguments = ["/bigfile"];
      Module.preInit = () => {
        FS.createLazyFile('/', "bigfile", "http://localhost:11111/bogus_file_path", true, false);
      };
      var doTrace = true;
      Module.print = (s) => self.postMessage({channel: "stdout", line: s});
      Module.printErr = (s) => { self.postMessage({channel: "stderr", char: s, trace: ((doTrace && s === 10) ? new Error().stack : null)}); doTrace = false; };
    """)
    self.compile_btest('checksummer.c', ['-g', '-sSMALL_XHR_CHUNKS', '-o', worker_filename,
                                         '--pre-js', 'worker_prejs.js'])
    chunkSize = 1024
    data = os.urandom(10 * chunkSize + 1) # 10 full chunks and one 1 byte chunk
    checksum = zlib.adler32(data) & 0xffffffff # Python 2 compatibility: force bigint

    server = HttpServerThread(make_test_chunked_synchronous_xhr_server(True, data, self.PORT))
    server.start()

    # block until the server is actually ready
    for i in range(60):
      try:
        urlopen('http://localhost:11111')
        break
      except Exception as e:
        print('(sleep for server)')
        time.sleep(1)
        if i == 60:
          raise e

    try:
      self.run_browser(main, '/report_result?' + str(checksum))
    finally:
      server.stop()
      server.join()
    # Avoid race condition on cleanup, wait a bit so that processes have released file locks so that test tearDown won't
    # attempt to rmdir() files in use.
    if WINDOWS:
      time.sleep(2)

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    # test that a program that doesn't use pthreads still works with with pthreads enabled
    # (regression test for https://github.com/emscripten-core/emscripten/pull/8059#issuecomment-488105672)
    'pthreads': (['-pthread'],),
  })
  def test_glgears(self, args):
    self.reftest('hello_world_gles.c', 'gears.png', reference_slack=3, cflags=['-DHAVE_BUILTIN_SINCOS', '-lGL', '-lglut'] + args)

  @requires_graphics_hardware
  def test_glgears_long(self):
    args = ['-DHAVE_BUILTIN_SINCOS', '-DLONGTEST', '-lGL', '-lglut', '-DANIMATE']
    self.btest('hello_world_gles.c', expected='0', cflags=args)

  @requires_graphics_hardware
  @parameterized({
    '': ('hello_world_gles.c',),
    'full': ('hello_world_gles_full.c',),
    'full_944': ('hello_world_gles_full_944.c',),
  })
  @flaky('https://github.com/emscripten-core/emscripten/issues/25329')
  def test_glgears_animation(self, filename):
    copy_asset('browser/fake_events.js')
    args = ['-o', 'something.html',
            '-DHAVE_BUILTIN_SINCOS', '-sGL_TESTING', '-lGL', '-lglut',
            '--shell-file', test_file('hello_world_gles_shell.html')]
    if 'full' in filename:
      args += ['-sFULL_ES2']
    self.compile_btest(filename, args)
    self.run_browser('something.html', '/report_gl_result?true')

  @requires_graphics_hardware
  def test_fulles2_sdlproc(self):
    self.btest_exit('full_es2_sdlproc.c', cflags=['-sGL_TESTING', '-DHAVE_BUILTIN_SINCOS', '-sFULL_ES2', '-lGL', '-lSDL', '-lglut', '-sGL_ENABLE_GET_PROC_ADDRESS', '-Wno-int-conversion'])

  @requires_graphics_hardware
  @flaky('https://github.com/emscripten-core/emscripten/issues/25329')
  def test_glgears_deriv(self):
    self.reftest('hello_world_gles_deriv.c', 'gears.png', reference_slack=2,
                 cflags=['-DHAVE_BUILTIN_SINCOS', '-lGL', '-lglut'])
    assert 'gl-matrix' not in read_file('test.html'), 'Should not include glMatrix when not needed'

  @requires_graphics_hardware
  @parameterized({
    'Hello_Triangle': ('CH02_HelloTriangle.o', [], []),
    'Simple_VertexShader': ('CH08_SimpleVertexShader.o', [], []),
    'Simple_Texture2D': ('CH09_SimpleTexture2D.o', [], []),
    'Simple_TextureCubemap': ('CH09_TextureCubemap.o', [], []),
    'TextureWrap': ('CH09_TextureWrap.o', [], []),
    'MultiTexture': ('CH10_MultiTexture.o', ['Chapter_10/MultiTexture/basemap.tga', 'Chapter_10/MultiTexture/lightmap.tga'], []),
    # run this individual test with optimizations and closure for more coverage
    'ParticleSystem': ('CH13_ParticleSystem.o', ['Chapter_13/ParticleSystem/smoke.tga'], ['-O2']),
  })
  def test_glbook(self, program, images, cflags):
    self.cflags.append('-Wno-int-conversion')
    self.cflags.append('-Wno-pointer-sign')

    libs = self.get_library('third_party/glbook', [
      'Chapter_2/Hello_Triangle/CH02_HelloTriangle.o',
      'Chapter_8/Simple_VertexShader/CH08_SimpleVertexShader.o',
      'Chapter_9/Simple_Texture2D/CH09_SimpleTexture2D.o',
      'Chapter_9/Simple_TextureCubemap/CH09_TextureCubemap.o',
      'Chapter_9/TextureWrap/CH09_TextureWrap.o',
      'Chapter_10/MultiTexture/CH10_MultiTexture.o',
      'Chapter_13/ParticleSystem/CH13_ParticleSystem.o',
    ], configure=None)

    def book_path(path):
      return test_file('third_party/glbook', path)

    cflags += ['-lGL', '-lEGL', '-lX11']
    for image in images:
      cflags += ['--preload-file', f'{book_path(image)}@{os.path.basename(image)}']

    lib = [l for l in libs if program in os.path.basename(l)][0]

    self.reftest(lib, book_path(program.replace('.o', '.png')), cflags=cflags)

  @requires_graphics_hardware
  @parameterized({
    'normal': (['-sFULL_ES2'],),
    # Enabling FULL_ES3 also enables ES2 automatically
    'full_es3': (['-sFULL_ES3'],),
  })
  def test_gles2_emulation(self, args):
    copy_asset('third_party/glbook/Chapter_10/MultiTexture/basemap.tga')
    copy_asset('third_party/glbook/Chapter_10/MultiTexture/lightmap.tga')
    copy_asset('third_party/glbook/Chapter_13/ParticleSystem/smoke.tga')

    for source, reference in [
      ('third_party/glbook/Chapter_2/Hello_Triangle/Hello_Triangle_orig.c', 'third_party/glbook/CH02_HelloTriangle.png'),
      # ('third_party/glbook/Chapter_8/Simple_VertexShader/Simple_VertexShader_orig.c', 'third_party/glbook/CH08_SimpleVertexShader.png'), # XXX needs INT extension in WebGL
      ('third_party/glbook/Chapter_9/TextureWrap/TextureWrap_orig.c', 'third_party/glbook/CH09_TextureWrap.png'),
      # ('third_party/glbook/Chapter_9/Simple_TextureCubemap/Simple_TextureCubemap_orig.c', 'third_party/glbook/CH09_TextureCubemap.png'), # XXX needs INT extension in WebGL
      ('third_party/glbook/Chapter_9/Simple_Texture2D/Simple_Texture2D_orig.c', 'third_party/glbook/CH09_SimpleTexture2D.png'),
      ('third_party/glbook/Chapter_10/MultiTexture/MultiTexture_orig.c', 'third_party/glbook/CH10_MultiTexture.png'),
      ('third_party/glbook/Chapter_13/ParticleSystem/ParticleSystem_orig.c', 'third_party/glbook/CH13_ParticleSystem.png'),
    ]:
      print(source)
      self.reftest(source, reference,
                   cflags=['-I' + test_file('third_party/glbook/Common'),
                              test_file('third_party/glbook/Common/esUtil.c'),
                              test_file('third_party/glbook/Common/esShader.c'),
                              test_file('third_party/glbook/Common/esShapes.c'),
                              test_file('third_party/glbook/Common/esTransform.c'),
                              '-lGL', '-lEGL', '-lX11', '-Wno-int-conversion', '-Wno-pointer-sign',
                              '--preload-file', 'basemap.tga', '--preload-file', 'lightmap.tga', '--preload-file', 'smoke.tga'] + args)

  @requires_webgl2
  def test_clientside_vertex_arrays_es3(self):
    self.reftest('clientside_vertex_arrays_es3.c', 'gl_triangle.png', cflags=['-sFULL_ES3', '-sUSE_GLFW=3', '-lglfw', '-lGLESv2'])

  def test_emscripten_api(self):
    self.btest_exit('emscripten_api_browser.c', cflags=['-lSDL'])

  @also_with_wasmfs
  def test_emscripten_async_load_script(self):
    def setup():
      create_file('script1.js', '''
        Module._set(456);
      ''')
      create_file('file1.txt', 'first')
      create_file('file2.txt', 'second')

    setup()
    with open('script2.js', 'w', encoding='utf-8') as f:
      self.run_process([FILE_PACKAGER, 'test.data', '--preload', 'file1.txt', 'file2.txt'], stdout=f)
    self.btest_exit('test_emscripten_async_load_script.c', cflags=['-sFORCE_FILESYSTEM'])

    # check using file packager to another dir
    self.clear()
    setup()
    ensure_dir('sub')
    with open('script2.js', 'w', encoding='utf-8') as f:
      self.run_process([FILE_PACKAGER, 'sub/test.data', '--preload', 'file1.txt', 'file2.txt'], stdout=f)
    shutil.copy(Path('sub/test.data'), '.')
    self.btest_exit('test_emscripten_async_load_script.c', cflags=['-sFORCE_FILESYSTEM'])

  @also_with_wasmfs
  def test_emscripten_overlapped_package(self):
    # test that a program that loads multiple file_packager.py packages has a correctly initialized filesystem.
    # this exercises https://github.com/emscripten-core/emscripten/issues/23602 whose root cause was a difference
    # between JS FS and WASMFS behavior.
    def setup():
      ensure_dir('sub')
      create_file('sub/file1.txt', 'first')
      create_file('sub/file2.txt', 'second')

    setup()
    with open('script1.js', 'w', encoding='utf-8') as f:
      self.run_process([FILE_PACKAGER, 'test.data', '--preload', 'sub/file1.txt@/target/file1.txt'], stdout=f)
    with open('script2.js', 'w', encoding='utf-8') as f:
      self.run_process([FILE_PACKAGER, 'test2.data', '--preload', 'sub/file2.txt@/target/file2.txt'], stdout=f)
    self.btest_exit('test_emscripten_overlapped_package.c', cflags=['-sFORCE_FILESYSTEM'])
    self.clear()

  def test_emscripten_api_infloop(self):
    self.btest_exit('emscripten_api_browser_infloop.cpp')

  @also_with_proxy_to_pthread
  def test_emscripten_main_loop(self):
    self.btest_exit('test_emscripten_main_loop.c')

  @parameterized({
    '': ([],),
    # test pthreads + AUTO_JS_LIBRARIES mode as well
    'pthreads': (['-pthread', '-sPROXY_TO_PTHREAD', '-sAUTO_JS_LIBRARIES=0'],),
  })
  def test_emscripten_main_loop_settimeout(self, args):
    self.btest_exit('test_emscripten_main_loop_settimeout.c', cflags=args)

  @also_with_proxy_to_pthread
  def test_emscripten_main_loop_and_blocker(self):
    self.btest_exit('test_emscripten_main_loop_and_blocker.c')

  def test_emscripten_main_loop_and_blocker_exit(self):
    # Same as above but tests that EXIT_RUNTIME works with emscripten_main_loop.  The
    # app should still stay alive until the loop ends
    self.btest_exit('test_emscripten_main_loop_and_blocker.c')

  @parameterized({
    '': ([],),
    'pthreads': (['-pthread', '-sPROXY_TO_PTHREAD'],),
    'strict': (['-sSTRICT'],),
  })
  def test_emscripten_main_loop_setimmediate(self, args):
    self.btest_exit('test_emscripten_main_loop_setimmediate.c', cflags=args)

  @parameterized({
    '': ([],),
    'O1': (['-O1'],),
  })
  def test_fs_after_main(self, args):
    self.btest_exit('test_fs_after_main.c', cflags=args)

  def test_sdl_quit(self):
    self.btest_exit('test_sdl_quit.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_resize(self):
    # FIXME(https://github.com/emscripten-core/emscripten/issues/12978)
    self.cflags.append('-Wno-deprecated-declarations')
    self.btest_exit('test_sdl_resize.c', cflags=['-lSDL', '-lGL'])

  def test_glshaderinfo(self):
    self.btest_exit('test_glshaderinfo.c', cflags=['-lGL', '-lglut'])

  @requires_graphics_hardware
  def test_glgetattachedshaders(self):
    self.btest('glgetattachedshaders.c', '1', cflags=['-lGL', '-lEGL'])

  # Covered by dEQP text suite (we can remove it later if we add coverage for that).
  @requires_graphics_hardware
  def test_glframebufferattachmentinfo(self):
    self.btest('glframebufferattachmentinfo.c', '1', cflags=['-lGLESv2', '-lEGL'])

  @requires_graphics_hardware
  def test_sdl_glshader(self):
    self.reftest('test_sdl_glshader.c', cflags=['-O2', '--closure=1', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '-sGL_ENABLE_GET_PROC_ADDRESS'])

  @requires_graphics_hardware
  def test_sdl_glshader2(self):
    self.btest_exit('test_sdl_glshader2.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '-sGL_ENABLE_GET_PROC_ADDRESS'])

  @requires_graphics_hardware
  def test_glteximage(self):
    self.btest('gl_teximage.c', '1', cflags=['-lGL', '-lSDL'])

  @parameterized({
    '': ([],),
    'pthreads': (['-pthread', '-sPROXY_TO_PTHREAD', '-sOFFSCREEN_FRAMEBUFFER'],),
    'pthreads_main_module': (['-pthread', '-sPROXY_TO_PTHREAD', '-sOFFSCREEN_FRAMEBUFFER', '-sMAIN_MODULE', '-Wno-experimental'],),
  })
  @requires_graphics_hardware
  def test_gl_textures(self, args):
    self.btest_exit('gl_textures.c', cflags=['-lGL', '-g', '-sSTACK_SIZE=1MB'] + args)

  @requires_graphics_hardware
  def test_gl_ps(self):
    # pointers and a shader
    copy_asset('browser/screenshot.png')
    self.reftest('gl_ps.c', cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '--use-preload-plugins'], reference_slack=1)

  @requires_graphics_hardware
  def test_gl_ps_packed(self):
    # packed data that needs to be strided
    copy_asset('browser/screenshot.png')
    self.reftest('gl_ps_packed.c', 'gl_ps.png', cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '--use-preload-plugins'], reference_slack=1)

  @requires_graphics_hardware
  def test_gl_ps_strides(self):
    copy_asset('browser/screenshot.png')
    self.reftest('gl_ps_strides.c', cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '--use-preload-plugins'])

  @requires_graphics_hardware
  def test_gl_ps_worker(self):
    copy_asset('browser/screenshot.png')
    self.reftest('gl_ps_worker.c', 'gl_ps.png', cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '--use-preload-plugins'], reference_slack=1)

  @requires_graphics_hardware
  def test_gl_renderers(self):
    self.reftest('gl_renderers.c', cflags=['-sGL_UNSAFE_OPTS=0', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @no_2gb('render fails')
  @no_4gb('render fails')
  def test_gl_stride(self):
    self.reftest('gl_stride.c', cflags=['-sGL_UNSAFE_OPTS=0', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_gl_vertex_buffer_pre(self):
    self.reftest('gl_vertex_buffer_pre.c', cflags=['-sGL_UNSAFE_OPTS=0', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_gl_vertex_buffer(self):
    self.reftest('gl_vertex_buffer.c', cflags=['-sGL_UNSAFE_OPTS=0', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'], reference_slack=1)

  @requires_graphics_hardware
  def test_gles2_uniform_arrays(self):
    self.btest_exit('test_gles2_uniform_arrays.c', cflags=['-sGL_ASSERTIONS', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_gles2_conformance(self):
    self.btest_exit('test_gles2_conformance.c', cflags=['-sGL_ASSERTIONS', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_matrix_identity(self):
    self.btest('gl_matrix_identity.c', expected=['-1882984448', '460451840', '1588195328', '2411982848'], cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre(self):
    self.reftest('third_party/cubegeom/cubegeom_pre.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre_regal(self):
    self.reftest('third_party/cubegeom/cubegeom_pre.c', cflags=['-sUSE_REGAL', '-DUSE_REGAL', '-lGL', '-lSDL', '-lc++', '-lc++abi'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre2(self):
    self.reftest('third_party/cubegeom/cubegeom_pre2.c', cflags=['-sGL_DEBUG', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL']) # some coverage for GL_DEBUG not breaking the build

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre3(self):
    self.reftest('third_party/cubegeom/cubegeom_pre3.c', 'third_party/cubegeom/cubegeom_pre2.png', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @parameterized({
    '': ([],),
    'tracing': (['-sTRACE_WEBGL_CALLS'],),
  })
  @requires_graphics_hardware
  def test_cubegeom(self, args):
    self.reftest('third_party/cubegeom/cubegeom.c', cflags=['-O2', '-g', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'] + args)

  @requires_graphics_hardware
  def test_cubegeom_regal(self):
    self.reftest('third_party/cubegeom/cubegeom.c', cflags=['-O2', '-g', '-DUSE_REGAL', '-sUSE_REGAL', '-lGL', '-lSDL', '-lc++', '-lc++abi'])

  @requires_graphics_hardware
  def test_cubegeom_regal_pthread(self):
    self.reftest('third_party/cubegeom/cubegeom.c', cflags=['-O2', '-g', '-pthread', '-DUSE_REGAL', '-pthread', '-sUSE_REGAL', '-lGL', '-lSDL', '-lc++', '-lc++abi'])

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'O1': (['-O1'],),
    # also test -Os in wasm, which uses meta-dce, which should not break
    # legacy gl emulation hacks
    'Os': (['-Os'],),
  })
  def test_cubegeom_proc(self, opts):
    create_file('side.c', r'''

extern void* SDL_GL_GetProcAddress(const char *);

void *glBindBuffer = 0; // same name as the gl function, to check that the collision does not break us

void *getBindBuffer() {
  if (!glBindBuffer) glBindBuffer = SDL_GL_GetProcAddress("glBindBuffer");
  return glBindBuffer;
}
''')
    self.reftest('third_party/cubegeom/cubegeom_proc.c', 'third_party/cubegeom/cubegeom.png', cflags=opts + ['side.c', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '-sGL_ENABLE_GET_PROC_ADDRESS'])

  @also_with_wasmfs
  @requires_graphics_hardware
  def test_cubegeom_glew(self):
    self.reftest('third_party/cubegeom/cubegeom_glew.c', 'third_party/cubegeom/cubegeom.png', cflags=['-O2', '--closure=1', '-sLEGACY_GL_EMULATION', '-lGL', '-lGLEW', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_color(self):
    self.reftest('third_party/cubegeom/cubegeom_color.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_normal(self):
    self.reftest('third_party/cubegeom/cubegeom_normal.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_normal_dap(self): # draw is given a direct pointer to clientside memory, no element array buffer
    self.reftest('third_party/cubegeom/cubegeom_normal_dap.c', 'third_party/cubegeom/cubegeom_normal.png', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_normal_dap_far(self): # indices do not start from 0
    self.reftest('third_party/cubegeom/cubegeom_normal_dap_far.c', 'third_party/cubegeom/cubegeom_normal.png', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_normal_dap_far_range(self): # glDrawRangeElements
    self.reftest('third_party/cubegeom/cubegeom_normal_dap_far_range.c', 'third_party/cubegeom/cubegeom_normal.png', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_normal_dap_far_glda(self): # use glDrawArrays
    self.reftest('third_party/cubegeom/cubegeom_normal_dap_far_glda.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @no_firefox('fails on CI but works locally')
  def test_cubegeom_normal_dap_far_glda_quad(self): # with quad
    self.reftest('third_party/cubegeom/cubegeom_normal_dap_far_glda_quad.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_mt(self):
    self.reftest('third_party/cubegeom/cubegeom_mt.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL']) # multitexture

  @requires_graphics_hardware
  def test_cubegeom_color2(self):
    self.reftest('third_party/cubegeom/cubegeom_color2.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_texturematrix(self):
    self.reftest('third_party/cubegeom/cubegeom_texturematrix.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cubegeom_fog(self):
    self.reftest('third_party/cubegeom/cubegeom_fog.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre_vao(self):
    self.reftest('third_party/cubegeom/cubegeom_pre_vao.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre_vao_regal(self):
    self.reftest('third_party/cubegeom/cubegeom_pre_vao.c', cflags=['-sUSE_REGAL', '-DUSE_REGAL', '-lGL', '-lSDL', '-lc++', '-lc++abi'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre2_vao(self):
    self.reftest('third_party/cubegeom/cubegeom_pre2_vao.c', 'third_party/cubegeom/cubegeom_pre_vao.png', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '-sGL_ENABLE_GET_PROC_ADDRESS'])

  @requires_graphics_hardware
  def test_cubegeom_pre2_vao2(self):
    self.reftest('third_party/cubegeom/cubegeom_pre2_vao2.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '-sGL_ENABLE_GET_PROC_ADDRESS'])

  @requires_graphics_hardware
  @no_swiftshader
  def test_cubegeom_pre_vao_es(self):
    self.reftest('third_party/cubegeom/cubegeom_pre_vao_es.c', 'third_party/cubegeom/cubegeom_pre_vao.png', cflags=['-sFULL_ES2', '-lGL', '-lSDL'])

  @requires_webgl2
  @no_swiftshader
  def test_cubegeom_row_length(self):
    self.reftest('third_party/cubegeom/cubegeom_pre_vao_es.c', 'third_party/cubegeom/cubegeom_pre_vao.png', cflags=['-sFULL_ES2', '-lGL', '-lSDL', '-DUSE_UNPACK_ROW_LENGTH', '-sMIN_WEBGL_VERSION=2'])

  @requires_graphics_hardware
  def test_cubegeom_u4fv_2(self):
    self.reftest('third_party/cubegeom/cubegeom_u4fv_2.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_cube_explosion(self):
    self.reftest('cube_explosion.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_glgettexenv(self):
    self.btest('glgettexenv.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'], expected='1')

  def test_sdl_canvas_blank(self):
    self.reftest('test_sdl_canvas_blank.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_canvas_palette(self):
    self.reftest('test_sdl_canvas_palette.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_canvas_twice(self):
    self.reftest('test_sdl_canvas_twice.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_set_clip_rect(self):
    self.reftest('test_sdl_set_clip_rect.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_maprgba(self):
    self.reftest('test_sdl_maprgba.c', cflags=['-lSDL', '-lGL'], reference_slack=3)

  def test_sdl_create_rgb_surface_from(self):
    self.reftest('test_sdl_create_rgb_surface_from.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_rotozoom(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl_rotozoom.c', cflags=['--preload-file', 'screenshot.png', '--use-preload-plugins', '-lSDL', '-lGL'], reference_slack=3)

  def test_sdl_gfx_primitives(self):
    self.reftest('test_sdl_gfx_primitives.c', cflags=['-lSDL', '-lGL'], reference_slack=1)

  def test_sdl_canvas_palette_2(self):
    create_file('pre.js', '''
      Module['preRun'] = () => {
        SDL.defaults.copyOnLock = false;
      };
    ''')

    create_file('args-r.js', '''
      Module['arguments'] = ['-r'];
    ''')

    create_file('args-g.js', '''
      Module['arguments'] = ['-g'];
    ''')

    create_file('args-b.js', '''
      Module['arguments'] = ['-b'];
    ''')

    self.reftest('test_sdl_canvas_palette_2.c', 'test_sdl_canvas_palette_r.png', cflags=['--pre-js', 'pre.js', '--pre-js', 'args-r.js', '-lSDL', '-lGL'])
    self.reftest('test_sdl_canvas_palette_2.c', 'test_sdl_canvas_palette_g.png', cflags=['--pre-js', 'pre.js', '--pre-js', 'args-g.js', '-lSDL', '-lGL'])
    self.reftest('test_sdl_canvas_palette_2.c', 'test_sdl_canvas_palette_b.png', cflags=['--pre-js', 'pre.js', '--pre-js', 'args-b.js', '-lSDL', '-lGL'])

  def test_sdl_ttf_render_text_solid(self):
    self.reftest('test_sdl_ttf_render_text_solid.c', cflags=['-O2', '-lSDL', '-lGL', '-Wno-experimental'])

  def test_sdl3_ttf_render_text_solid(self):
    self.cflags.append('-Wno-experimental')
    shutil.copy2(test_file('freetype/LiberationSansBold.ttf'), self.get_dir())
    self.reftest('test_sdl3_ttf_render_text_solid.c', 'test_sdl3_ttf_render_text_solid.png',
                 cflags=[
                  '-O2', '-sUSE_SDL=3', '-sUSE_SDL_TTF=3', '-lGL', '-Wno-experimental',
                  '--embed-file', 'LiberationSansBold.ttf'])

  def test_sdl_alloctext(self):
    self.btest_exit('test_sdl_alloctext.c', cflags=['-lSDL', '-lGL'])

  def test_sdl_surface_refcount(self):
    self.btest_exit('test_sdl_surface_refcount.c', cflags=['-lSDL'])

  def test_sdl_free_screen(self):
    self.reftest('test_sdl_free_screen.c', 'browser/htmltest.png', cflags=['-lSDL', '-lGL'])

  @requires_graphics_hardware
  def test_glbegin_points(self):
    copy_asset('browser/screenshot.png')
    self.reftest('glbegin_points.c', cflags=['--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '--use-preload-plugins'])

  @requires_graphics_hardware
  def test_gl_s3tc(self):
    copy_asset('browser/screenshot.dds')
    self.reftest('s3tc.c', cflags=['--preload-file', 'screenshot.dds', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_gl_s3tc_ffp_only(self):
    copy_asset('browser/screenshot.dds')
    self.reftest('s3tc.c', cflags=['--preload-file', 'screenshot.dds', '-sLEGACY_GL_EMULATION', '-sGL_FFP_ONLY', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'subimage': (['-DTEST_TEXSUBIMAGE'],),
  })
  def test_gl_anisotropic(self, args):
    copy_asset('browser/water.dds')
    self.reftest('test_gl_anisotropic.c', reference_slack=2, cflags=['--preload-file', 'water.dds', '-sLEGACY_GL_EMULATION', '-lGL', '-lSDL', '-Wno-incompatible-pointer-types'] + args)

  @requires_graphics_hardware
  def test_gl_tex_nonbyte(self):
    self.reftest('tex_nonbyte.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_gl_float_tex(self):
    self.reftest('test_gl_float_tex.c', cflags=['-lGL', '-lglut'])

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'tracing': (['-sTRACE_WEBGL_CALLS'],),
    'es2': (['-sMIN_WEBGL_VERSION=2', '-sFULL_ES2', '-sWEBGL2_BACKWARDS_COMPATIBILITY_EMULATION'],),
    'es2_tracing': (['-sMIN_WEBGL_VERSION=2', '-sFULL_ES2', '-sWEBGL2_BACKWARDS_COMPATIBILITY_EMULATION', '-sTRACE_WEBGL_CALLS'],),
  })
  def test_gl_subdata(self, args):
    if '-sMIN_WEBGL_VERSION=2' in args and webgl2_disabled():
      self.skipTest('This test requires WebGL2 to be available')
    if self.is_4gb() and '-sMIN_WEBGL_VERSION=2' in args:
      self.skipTest('texSubImage2D fails: https://crbug.com/325090165')
    self.reftest('test_gl_subdata.c', 'test_gl_float_tex.png', cflags=['-lGL', '-lglut'] + args)

  @requires_graphics_hardware
  def test_gl_perspective(self):
    self.reftest('perspective.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL', '-lSDL'])

  @requires_graphics_hardware
  def test_gl_error(self):
    self.btest_exit('gl_error.c', cflags=['-sLEGACY_GL_EMULATION', '-lGL'])

  @parameterized({
    '': ([],),
    'strict': (['-lopenal', '-sSTRICT'],),
    'closure': (['--closure=1'],),
  })
  def test_openal_error(self, args):
    self.btest_exit('openal/test_openal_error.c', cflags=args)

  @requires_microphone_access
  def test_openal_capture_sanity(self):
    self.btest_exit('openal/test_openal_capture_sanity.c')

  def test_openal_extensions(self):
    self.btest_exit('openal/test_openal_extensions.c')

  def test_openal_playback(self):
    copy_asset('sounds/audio.wav')
    self.btest_exit('openal/test_openal_playback.c', cflags=['-O2', '--preload-file', 'audio.wav'])

  def test_openal_buffers(self):
    self.btest_exit('openal/test_openal_buffers.c', cflags=['--preload-file', test_file('sounds/the_entertainer.wav') + '@/'])

  def test_runtimelink(self):
    create_file('header.h', r'''
      struct point {
        int x, y;
      };
    ''')

    create_file('supp.c', r'''
      #include <stdio.h>
      #include "header.h"

      extern void mainFunc(int x);
      extern int mainInt;

      void suppFunc(struct point *p) {
        printf("supp: %d,%d\n", p->x, p->y);
        mainFunc(p->x + p->y);
        printf("supp see: %d\n", mainInt);
      }

      int suppInt = 76;
    ''')

    create_file('main.c', r'''
      #include <stdio.h>
      #include <assert.h>
      #include "header.h"

      extern void suppFunc(struct point *p);
      extern int suppInt;

      void mainFunc(int x) {
        printf("main: %d\n", x);
        assert(x == 56);
      }

      int mainInt = 543;

      int main( int argc, const char *argv[] ) {
        struct point p = { 54, 2 };
        suppFunc(&p);
        printf("main see: %d\nok.\n", suppInt);
        assert(suppInt == 76);
        return 0;
      }
    ''')
    self.run_process([EMCC, 'supp.c', '-o', 'supp.wasm', '-sSIDE_MODULE', '-O2'] + self.get_cflags())
    self.btest_exit('main.c', cflags=['-sMAIN_MODULE=2', '-O2', 'supp.wasm'])

  @also_with_wasm2js
  def test_pre_run_deps(self):
    # Adding a dependency in preRun will delay run
    self.set_setting('DEFAULT_LIBRARY_FUNCS_TO_INCLUDE', '$addRunDependency')
    create_file('pre.js', '''
      Module.preRun = () => {
        addRunDependency('foo');
        out('preRun called, added a dependency...');
        setTimeout(function() {
          Module.okk = 10;
          removeRunDependency('foo')
        }, 2000);
      };
    ''')

    self.btest('test_pre_run_deps.c', expected='10', cflags=['--pre-js', 'pre.js'])

  @also_with_wasm2js
  @parameterized({
    '': ([], '600'),
    'no_main': (['-DNO_MAIN', '--pre-js', 'pre_runtime.js'], '601'), # 601, because no main means we *do* run another call after exit()
  })
  def test_runtime_misuse(self, extra_args, second_code):
    self.set_setting('DEFAULT_LIBRARY_FUNCS_TO_INCLUDE', '$ccall,$cwrap')
    post_prep = '''
      var expected_ok = false;
      function doCcall(n) {
        ccall('note', 'string', ['number'], [n]);
      }
      var wrapped = cwrap('note', 'string', ['number']); // returns a string to suppress cwrap optimization
      function doCwrapCall(n) {
        var str = wrapped(n);
        out('got ' + str);
        assert(str === 'silly-string');
      }
      function doDirectCall(n) {
        Module['_note'](n);
      }
    '''
    post_test = '''
      var ok = false;
      try {
        doCcall(1);
        ok = true; // should fail and not reach here, runtime is not ready yet so ccall will abort
      } catch(e) {
        out('expected fail 1:', e);
        assert(e.toString().includes('Assertion failed')); // assertion, not something else
        ABORT = false; // hackish
      }
      assert(ok === expected_ok);

      ok = false;
      try {
        doCwrapCall(2);
        ok = true; // should fail and not reach here, runtime is not ready yet so cwrap call will abort
      } catch(e) {
        out('expected fail 2:', e);
        assert(e.toString().includes('Assertion failed')); // assertion, not something else
        ABORT = false; // hackish
      }
      assert(ok === expected_ok);

      ok = false;
      try {
        doDirectCall(3);
        ok = true; // should fail and not reach here, runtime is not ready yet so any code execution
      } catch(e) {
        out('expected fail 3:', e);
        assert(e.toString().includes('Assertion failed')); // assertion, not something else
        ABORT = false; // hackish
      }
      assert(ok === expected_ok);
    '''

    post_hook = r'''
      function myJSCallback() {
        // Run on the next event loop, as code may run in a postRun right after main().
        setTimeout(async () => {
          out('done timeout noted = ' + Module.noted);
          assert(Module.noted);
          await fetch('/report_result?' + HEAP32[Module.noted/4]);
          window.close();
        }, 0);
        // called from main, this is an ok time
        doCcall(100);
        doCwrapCall(200);
        doDirectCall(300);
      }
    '''

    create_file('pre_runtime.js', r'''
      Module.onRuntimeInitialized = myJSCallback;
    ''')

    print('mem init, so async, call too early')
    create_file('post.js', post_prep + post_test + post_hook)
    self.btest('test_runtime_misuse.c', expected='600', cflags=['--post-js', 'post.js', '-sEXIT_RUNTIME'] + extra_args, reporting=Reporting.NONE)

    print('sync startup, call too late')
    create_file('post.js', post_prep + 'Module.postRun = () => { ' + post_test + ' };' + post_hook)
    self.btest('test_runtime_misuse.c', expected=second_code, cflags=['--post-js', 'post.js', '-sEXIT_RUNTIME'] + extra_args, reporting=Reporting.NONE)

    print('sync, runtime still alive, so all good')
    create_file('post.js', post_prep + 'expected_ok = true; Module.postRun = () => { ' + post_test + ' };' + post_hook)
    self.btest('test_runtime_misuse.c', expected='606', cflags=['--post-js', 'post.js'] + extra_args, reporting=Reporting.NONE)

  def test_cwrap_early(self):
    self.btest('browser/test_cwrap_early.c', cflags=['-O2', '-sASSERTIONS', '--pre-js', test_file('browser/test_cwrap_early.js'), '-sEXPORTED_RUNTIME_METHODS=cwrap'], expected='0')

  @no_wasm64('TODO: wasm64 + BUILD_AS_WORKER')
  def test_worker_api(self):
    self.compile_btest('worker_api_worker.cpp', ['-o', 'worker.js', '-sBUILD_AS_WORKER', '-sEXPORTED_FUNCTIONS=_one'])
    self.btest('worker_api_main.cpp', expected='566')

  @no_wasm64('TODO: wasm64 + BUILD_AS_WORKER')
  def test_worker_api_2(self):
    self.compile_btest('worker_api_2_worker.cpp', ['-o', 'worker.js', '-sBUILD_AS_WORKER', '-O2', '--minify=0', '-sEXPORTED_FUNCTIONS=_one,_two,_three,_four', '--closure=1'])
    self.btest('worker_api_2_main.cpp', cflags=['-O2', '--minify=0'], expected='11')

  @no_wasm64('TODO: wasm64 + BUILD_AS_WORKER')
  def test_worker_api_3(self):
    self.compile_btest('worker_api_3_worker.cpp', ['-o', 'worker.js', '-sBUILD_AS_WORKER', '-sEXPORTED_FUNCTIONS=_one'])
    self.btest('worker_api_3_main.cpp', expected='5')

  @no_wasm64('TODO: wasm64 + BUILD_AS_WORKER')
  def test_worker_api_sleep(self):
    self.compile_btest('worker_api_worker_sleep.cpp', ['-o', 'worker.js', '-sBUILD_AS_WORKER', '-sEXPORTED_FUNCTIONS=_one', '-sASYNCIFY'])
    self.btest('worker_api_main.cpp', expected='566')

  @no_wasm64('TODO: wasm64 + BUILD_AS_WORKER')
  def test_worker_api_with_pthread_compilation_fails(self):
    self.run_process([EMCC, '-c', '-o', 'hello.o', test_file('hello_world.c')])
    expected = "pthreads + BUILD_AS_WORKER require separate modes that don't work together, see https://github.com/emscripten-core/emscripten/issues/8854"
    self.assert_fail([EMCC, 'hello.o', '-o', 'a.js', '-g', '--closure=1', '-pthread', '-sBUILD_AS_WORKER'], expected)

  @also_with_wasmfs
  def test_wget(self):
    create_file('test.txt', 'emscripten')
    self.btest_exit('test_wget.c', cflags=['-sASYNCIFY'])

  def test_wget_data(self):
    create_file('test.txt', 'emscripten')
    self.btest_exit('test_wget_data.c', cflags=['-O2', '-g2', '-sASYNCIFY'])

  @also_with_wasmfs
  @parameterized({
    '': ([],),
    'O2': (['-O2'],),
  })
  def test_emscripten_async_wget(self, args):
    copy_asset('browser/screenshot.png') # preloaded *after* run
    self.btest_exit('test_emscripten_async_wget.c', cflags=['-lSDL'] + args)

  @also_with_wasmfs
  def test_emscripten_async_wget2(self):
    self.btest_exit('test_emscripten_async_wget2.cpp')

  @disabled('https://github.com/emscripten-core/emscripten/issues/15818')
  def test_emscripten_async_wget2_data(self):
    create_file('hello.txt', 'Hello Emscripten!')
    self.btest('test_emscripten_async_wget2_data.cpp', expected='0')

  def test_emscripten_async_wget_side_module(self):
    self.emcc('test_emscripten_async_wget_side_module.c', ['-o', 'lib.wasm', '-O2', '-sSIDE_MODULE'])
    self.btest_exit('test_emscripten_async_wget_main_module.c', cflags=['-O2', '-sMAIN_MODULE=2'])

  @parameterized({
    '': ([],),
    'lz4': (['-sLZ4'],),
  })
  def test_preload_module(self, args):
    create_file('library.c', r'''
      #include <stdio.h>
      int library_func() {
        return 42;
      }
    ''')
    self.emcc('library.c', ['-sSIDE_MODULE', '-O2', '-o', 'library.so'])
    create_file('main.c', r'''
      #include <assert.h>
      #include <dlfcn.h>
      #include <stdio.h>
      #include <emscripten.h>
      int main() {
        int found = EM_ASM_INT(
          return preloadedWasm['/library.so'] !== undefined;
        );
        assert(found);
        void *lib_handle = dlopen("/library.so", RTLD_NOW);
        assert(lib_handle);
        typedef int (*voidfunc)();
        voidfunc x = (voidfunc)dlsym(lib_handle, "library_func");
        assert(x);
        assert(x() == 42);
        return 0;
      }
    ''')
    self.btest_exit(
      'main.c',
      cflags=['-sMAIN_MODULE=2', '--preload-file', '.@/', '--use-preload-plugins'] + args)

  # This does not actually verify anything except that --cpuprofiler and --memoryprofiler compiles.
  # Run interactive.test_cpuprofiler_memoryprofiler for interactive testing.
  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'modularized': (['-sMODULARIZE'],),
  })
  def test_cpuprofiler_memoryprofiler(self, opts):
    self.btest_exit('hello_world_gles.c', cflags=['-DLONGTEST=1', '-DTEST_MEMORYPROFILER_ALLOCATIONS_MAP=1', '--cpuprofiler', '--memoryprofiler', '-lGL', '-lglut', '-DANIMATE'] + opts)

  def test_uuid(self):
    self.btest_exit('test_uuid.c', cflags=['-luuid'])

  @requires_graphics_hardware
  def test_glew(self):
    self.btest('glew.c', cflags=['-lGL', '-lSDL', '-lGLEW'], expected='1')
    self.btest('glew.c', cflags=['-lGL', '-lSDL', '-lGLEW', '-sLEGACY_GL_EMULATION'], expected='1')
    self.btest('glew.c', cflags=['-lGL', '-lSDL', '-lGLEW', '-DGLEW_MX'], expected='1')
    self.btest('glew.c', cflags=['-lGL', '-lSDL', '-lGLEW', '-sLEGACY_GL_EMULATION', '-DGLEW_MX'], expected='1')

  def test_doublestart_bug(self):
    self.set_setting('DEFAULT_LIBRARY_FUNCS_TO_INCLUDE', '$addRunDependency,$removeRunDependency')
    create_file('pre.js', r'''
Module["preRun"] = () => {
  addRunDependency('test_run_dependency');
  removeRunDependency('test_run_dependency');
};
''')

    self.btest('doublestart.c', cflags=['--pre-js', 'pre.js'], expected='1')

  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1', '-sHTML5_SUPPORT_DEFERRING_USER_SENSITIVE_REQUESTS=0'],),
    'pthread': (['-pthread'],),
    'proxy_to_pthread': (['-pthread', '-sPROXY_TO_PTHREAD'],),
    'legacy': (['-sMIN_FIREFOX_VERSION=0', '-sMIN_SAFARI_VERSION=0', '-sMIN_CHROME_VERSION=0', '-Wno-transpile'],),
  })
  def test_html5_core(self, opts):
    if self.is_wasm64() and '-sMIN_CHROME_VERSION=0' in opts:
      self.skipTest('wasm64 does not support older browsers')
    if '-sHTML5_SUPPORT_DEFERRING_USER_SENSITIVE_REQUESTS=0' in opts:
      # In this mode an exception can be thrown by the browser, and we don't
      # want the test to fail in that case so we override the error handling.
      create_file('pre.js', '''
      globalThis.disableErrorReporting = true;
      window.addEventListener('error', (event) => {
        if (!event.message.includes('exception:fullscreen error')) {
          reportTopLevelError(event);
        }
      });
      ''')
      self.cflags.append('--pre-js=pre.js')
    self.btest_exit('test_html5_core.c', cflags=opts)

  def test_html5_remove_event_listener(self):
    self.btest_exit('test_html5_remove_event_listener.c')

  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1'],),
    'pthread': (['-pthread', '-sPROXY_TO_PTHREAD'],),
  })
  def test_html5_gamepad(self, args):
    self.btest_exit('test_html5_gamepad.c', cflags=args)

  def test_html5_unknown_event_target(self):
    self.btest_exit('test_html5_unknown_event_target.c')

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1'],),
    'full_es2': (['-sFULL_ES2'],),
  })
  def test_html5_webgl_create_context_no_antialias(self, args):
    self.btest_exit('webgl_create_context.cpp', cflags=args + ['-DNO_ANTIALIAS', '-lGL'])

  # This test supersedes the one above, but it's skipped in the CI because anti-aliasing is not well supported by the Mesa software renderer.
  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1'],),
    'full_es2': (['-sFULL_ES2', '-DFULL_ES2', '-sGL_ASSERTIONS'],),
    'pthread': (['-pthread'],),
  })
  def test_html5_webgl_create_context(self, args):
    self.btest_exit('webgl_create_context.cpp', cflags=args + ['-lGL'])

  @requires_graphics_hardware
  # Verify bug https://github.com/emscripten-core/emscripten/issues/4556: creating a WebGL context to Module.canvas without an ID explicitly assigned to it.
  def test_html5_webgl_create_context2(self):
    self.btest_exit('webgl_create_context2.c')

  @requires_graphics_hardware
  @requires_offscreen_canvas
  # Verify bug https://github.com/emscripten-core/emscripten/issues/22943: creating a WebGL context with explicit swap control and offscreenCanvas
  @parameterized({
    'offscreencanvas': (['-sOFFSCREENCANVAS_SUPPORT'],),
    'offscreenframebuffer': (['-sOFFSCREEN_FRAMEBUFFER', '-DUSE_OFFSCREEN_FRAMEBUFFER'],),
  })
  def test_html5_webgl_create_context_swapcontrol(self, args):
    self.btest_exit('browser/webgl_create_context_swapcontrol.c', cflags=args)

  @requires_graphics_hardware
  # Verify bug https://github.com/emscripten-core/emscripten/issues/4556: creating a WebGL context to Module.canvas without an ID explicitly assigned to it.
  # (this only makes sense in the old deprecated -sDISABLE_DEPRECATED_FIND_EVENT_TARGET_BEHAVIOR=0 mode)
  def test_html5_special_event_targets(self):
    self.btest_exit('html5_special_event_targets.cpp', cflags=['-lGL'])

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'O2': (['-O2', '-g1'],),
    'full_es2': (['-sFULL_ES2'],),
  })
  def test_html5_webgl_destroy_context(self, args):
    self.btest_exit('webgl_destroy_context.c', cflags=args + ['--shell-file', test_file('browser/webgl_destroy_context_shell.html'), '-lGL'])

  @requires_graphics_hardware
  def test_webgl_context_params(self):
    self.btest_exit('webgl_color_buffer_readpixels.c', cflags=['-lGL'])

  # Test for PR#5373 (https://github.com/emscripten-core/emscripten/pull/5373)
  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'full_es2': (['-sFULL_ES2'],),
  })
  def test_webgl_shader_source_length(self, args):
    self.btest_exit('webgl_shader_source_length.c', cflags=args + ['-lGL'])

  # Tests calling glGetString(GL_UNMASKED_VENDOR_WEBGL).
  @requires_graphics_hardware
  def test_webgl_unmasked_vendor_webgl(self):
    self.btest_exit('webgl_unmasked_vendor_webgl.c', cflags=['-lGL'])

  @requires_graphics_hardware
  @parameterized({
    'legacy_browser': (['-sMIN_CHROME_VERSION=0', '-Wno-transpile'],),
    'closure': (['-O2', '-g1', '--closure=1'],),
    'full_es2': (['-sFULL_ES2'],),
  })
  def test_webgl2(self, args):
    if '-sMIN_CHROME_VERSION=0' in args and self.is_wasm64():
      self.skipTest('wasm64 not supported by legacy browsers')
    self.btest_exit('webgl2.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'] + args)

  # Tests the WebGL 2 glGetBufferSubData() functionality.
  @requires_webgl2
  @no_4gb('getBufferSubData fails: https://crbug.com/325090165')
  def test_webgl2_get_buffer_sub_data(self):
    self.btest_exit('webgl2_get_buffer_sub_data.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @requires_graphics_hardware
  def test_webgl2_pthreads(self):
    # test that a program can be compiled with pthreads and render WebGL2 properly on the main thread
    # (the testcase doesn't even use threads, but is compiled with thread support).
    self.btest_exit('webgl2.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL', '-pthread'])

  @requires_graphics_hardware
  def test_webgl2_objects(self):
    self.btest_exit('webgl2_objects.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @requires_webgl2
  @requires_offscreen_canvas
  @parameterized({
    '': ([],),
    'offscreencanvas_pthread': (['-sOFFSCREENCANVAS_SUPPORT', '-pthread', '-sPROXY_TO_PTHREAD'],),
    'offscreenframebuffer_pthread': (['-sOFFSCREEN_FRAMEBUFFER', '-pthread', '-sPROXY_TO_PTHREAD'],),
  })
  def test_html5_webgl_api(self, args):
    if '-sOFFSCREENCANVAS_SUPPORT' in args and os.getenv('EMTEST_LACKS_OFFSCREEN_CANVAS'):
      return
    self.btest_exit('html5_webgl.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'] + args)

  @parameterized({
    'webgl1': (['-DWEBGL_VERSION=1'],),
    'webgl2': (['-sMAX_WEBGL_VERSION=2', '-DWEBGL_VERSION=2'],),
    'webgl1_extensions': (['-DWEBGL_VERSION=1', '-sGL_EXPLICIT_UNIFORM_LOCATION'],),
    'webgl2_extensions': (['-sMAX_WEBGL_VERSION=2', '-DWEBGL_VERSION=2', '-sGL_EXPLICIT_UNIFORM_LOCATION', '-sGL_EXPLICIT_UNIFORM_BINDING'],),
  })
  @requires_graphics_hardware
  def test_webgl_preprocessor_variables(self, opts):
    if '-DWEBGL_VERSION=2' in opts and webgl2_disabled():
      self.skipTest('This test requires WebGL2 to be available')
    self.btest_exit('webgl_preprocessor_variables.c', cflags=['-lGL'] + opts)

  @requires_graphics_hardware
  def test_webgl2_ubos(self):
    self.btest_exit('webgl2_ubos.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'webgl2': (['-sMAX_WEBGL_VERSION=2', '-DTEST_WEBGL2=1'],),
  })
  def test_webgl2_garbage_free_entrypoints(self, args):
    if '-DTEST_WEBGL2=1' in args and webgl2_disabled():
      self.skipTest('This test requires WebGL2 to be available')
    if args and self.is_4gb():
      self.skipTest('readPixels fails: https://crbug.com/324992397')
    self.btest_exit('webgl2_garbage_free_entrypoints.c', cflags=args)

  @requires_webgl2
  def test_webgl2_backwards_compatibility_emulation(self):
    self.btest_exit('webgl2_backwards_compatibility_emulation.c', cflags=['-sMAX_WEBGL_VERSION=2', '-sWEBGL2_BACKWARDS_COMPATIBILITY_EMULATION'])

  @requires_graphics_hardware
  def test_webgl2_runtime_no_context(self):
    # tests that if we support WebGL1 and 2, and WebGL2RenderingContext exists,
    # but context creation fails, that we can then manually try to create a
    # WebGL1 context and succeed.
    self.btest_exit('test_webgl2_runtime_no_context.cpp', cflags=['-sMAX_WEBGL_VERSION=2'])

  @requires_graphics_hardware
  def test_webgl_context_major_version(self):
    # testing that majorVersion accepts only valid values
    self.btest('test_webgl_context_major_version.c', expected='abort:Expected Error: Invalid WebGL version requested: 0', cflags=['-lGL', '-DWEBGL_CONTEXT_MAJOR_VERSION=0'])
    self.btest('test_webgl_context_major_version.c', expected='abort:Expected Error: Invalid WebGL version requested: 3', cflags=['-lGL', '-DWEBGL_CONTEXT_MAJOR_VERSION=3'])

    # no linker flag (equivalent to -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=1) => only 1 allowed
    self.btest_exit('test_webgl_context_major_version.c', cflags=['-lGL', '-DWEBGL_CONTEXT_MAJOR_VERSION=1'])
    self.btest('test_webgl_context_major_version.c', expected='abort:Expected Error: WebGL 2 requested but only WebGL 1 is supported (set -sMAX_WEBGL_VERSION=2 to fix the problem)', cflags=['-lGL', '-DWEBGL_CONTEXT_MAJOR_VERSION=2'])

    # -sMIN_WEBGL_VERSION=2 => only 2 allowed
    self.btest('test_webgl_context_major_version.c', expected='abort:Expected Error: WebGL 1 requested but only WebGL 2 is supported (MIN_WEBGL_VERSION is 2)', cflags=['-lGL', '-sMIN_WEBGL_VERSION=2', '-DWEBGL_CONTEXT_MAJOR_VERSION=1'])

  @requires_webgl2
  def test_webgl_context_major_version_webgl2(self):
    self.btest_exit('test_webgl_context_major_version.c', cflags=['-lGL', '-sMIN_WEBGL_VERSION=2', '-DWEBGL_CONTEXT_MAJOR_VERSION=2'])

    # -sMAX_WEBGL_VERSION=2 => 1 and 2 are ok
    self.btest_exit('test_webgl_context_major_version.c', cflags=['-lGL', '-sMAX_WEBGL_VERSION=2', '-DWEBGL_CONTEXT_MAJOR_VERSION=1'])
    self.btest_exit('test_webgl_context_major_version.c', cflags=['-lGL', '-sMAX_WEBGL_VERSION=2', '-DWEBGL_CONTEXT_MAJOR_VERSION=2'])

  @requires_webgl2
  def test_webgl2_invalid_teximage2d_type(self):
    self.btest_exit('webgl2_invalid_teximage2d_type.c', cflags=['-sMAX_WEBGL_VERSION=2'])

  @requires_graphics_hardware
  def test_webgl_with_closure(self):
    self.btest_exit('webgl_with_closure.c', cflags=['-O2', '-sMAX_WEBGL_VERSION=2', '--closure=1', '-lGL'])

  # Tests that -sGL_ASSERTIONS and glVertexAttribPointer with packed types works
  @requires_webgl2
  def test_webgl2_packed_types(self):
    self.btest_exit('webgl2_draw_packed_triangle.c', cflags=['-lGL', '-sMAX_WEBGL_VERSION=2', '-sGL_ASSERTIONS'])

  @requires_graphics_hardware
  @no_4gb('compressedTexSubImage2D fails: https://crbug.com/324562920')
  def test_webgl2_pbo(self):
    self.btest_exit('webgl2_pbo.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @no_firefox('fails on CI likely due to GPU drivers there')
  @no_safari('TODO: Fails with report_result?5') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  @requires_graphics_hardware
  def test_webgl2_sokol_mipmap(self):
    self.reftest('third_party/sokol/mipmap-emsc.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL', '-O1'], reference_slack=2)

  @no_firefox('fails on CI likely due to GPU drivers there')
  @no_4gb('fails to render')
  @requires_graphics_hardware
  def test_webgl2_sokol_mrt(self):
    self.reftest('third_party/sokol/mrt-emcc.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @requires_webgl2
  @no_4gb('fails to render')
  def test_webgl2_sokol_arraytex(self):
    self.reftest('third_party/sokol/arraytex-emsc.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1'],),
  })
  def test_sdl_touch(self, opts):
    self.btest_exit('test_sdl_touch.c', cflags=opts + ['-DAUTOMATE_SUCCESS=1', '-lSDL', '-lGL'])

  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1'],),
  })
  def test_html5_mouse(self, opts):
    self.btest_exit('test_html5_mouse.c', cflags=opts + ['-DAUTOMATE_SUCCESS=1'])

  @parameterized({
    '': ([],),
    'closure': (['-O2', '-g1', '--closure=1'],),
  })
  def test_sdl_mousewheel(self, opts):
    self.btest_exit('test_sdl_mousewheel.c', cflags=opts + ['-DAUTOMATE_SUCCESS=1', '-lSDL', '-lGL'])

  @also_with_wasm2js
  @parameterized({
    '': ([],),
    'es6': (['-sEXPORT_ES6'],),
  })
  def test_locate_file(self, args):
    self.set_setting('EXIT_RUNTIME')
    create_file('src.c', r'''
      #include <stdio.h>
      #include <string.h>
      #include <assert.h>
      int main() {
        FILE *f = fopen("data.txt", "r");
        assert(f && "could not open file");
        char buf[100];
        int num = fread(buf, 1, 20, f);
        assert(num == 20 && "could not read 20 bytes");
        buf[20] = 0;
        fclose(f);
        printf("|%s|\n", buf);
        assert(strcmp("load me right before", buf) == 0);
        return 0;
      }
    ''')
    create_file('data.txt', 'load me right before...')
    create_file('pre.js', 'Module.locateFile = (x) => "sub/" + x;')
    self.run_process([FILE_PACKAGER, 'test.data', '--preload', 'data.txt'], stdout=open('data.js', 'w', encoding='utf-8'))
    # put pre.js first, then the file packager data, so locateFile is there for the file loading code
    self.compile_btest('src.c', ['-O2', '-g', '--pre-js', 'pre.js', '--pre-js', 'data.js', '-o', 'page.html', '-sFORCE_FILESYSTEM'] + args, reporting=Reporting.JS_ONLY)
    ensure_dir('sub')
    if self.is_wasm():
      shutil.move('page.wasm', Path('sub/page.wasm'))
    shutil.move('test.data', Path('sub/test.data'))
    self.run_browser('page.html', '/report_result?exit:0')

    # alternatively, put locateFile in the HTML
    print('in html')

    create_file('shell.html', '''
      <body>
        <script>
          var Module = {
            locateFile: (x) => "sub/" + x,
          };
        </script>

        {{{ SCRIPT }}}
      </body>
    ''')

    def in_html(expected):
      self.compile_btest('src.c', ['-O2', '-g', '--shell-file', 'shell.html', '--pre-js', 'data.js', '-o', 'page.html', '-sSAFE_HEAP', '-sASSERTIONS', '-sFORCE_FILESYSTEM'] + args, reporting=Reporting.JS_ONLY)
      if self.is_wasm():
        shutil.move('page.wasm', Path('sub/page.wasm'))
      self.run_browser('page.html', '/report_result?exit:' + expected)

    in_html('0')

  @requires_graphics_hardware
  def test_glfw3_default_hints(self):
    self.btest_exit('test_glfw3_default_hints.c', cflags=['-sUSE_GLFW=3', '-lglfw', '-lGL'])

  @requires_graphics_hardware
  @parameterized({
    '': (['-DCLIENT_API=GLFW_OPENGL_ES_API', '-sGL_ENABLE_GET_PROC_ADDRESS'],),
    'no_gl': (['-DCLIENT_API=GLFW_NO_API'],),
  })
  @parameterized({
    '': ([],),
    'legacy': (['-sLEGACY_GL_EMULATION'],),
    'closure': (['-Os', '--closure=1'],),
  })
  def test_glfw3(self, args, opts):
    self.btest_exit('test_glfw3.c', cflags=['-sUSE_GLFW=3', '-lglfw', '-lGL'] + args + opts)

  @parameterized({
    '': (['-sUSE_GLFW=2', '-DUSE_GLFW=2'],),
    'glfw3': (['-sUSE_GLFW=3', '-DUSE_GLFW=3'],),
  })
  @requires_graphics_hardware
  def test_glfw_events(self, args):
    self.btest_exit('test_glfw_events.c', cflags=args + ['-lglfw', '-lGL', '--pre-js', test_file('browser/fake_events.js')])

  @requires_graphics_hardware
  def test_glfw3_hi_dpi_aware(self):
    self.btest_exit('test_glfw3_hi_dpi_aware.c', cflags=['-sUSE_GLFW=3', '-lGL'])

  @requires_graphics_hardware
  def test_glfw3_css_scaling(self):
    self.btest_exit('test_glfw3_css_scaling.c', cflags=['-sUSE_GLFW=3'])

  @requires_graphics_hardware
  @also_with_wasm2js
  def test_sdl2_image(self):
    # load an image file, get pixel data. Also O2 coverage for --preload-file
    copy_asset('browser/screenshot.jpg')

    for dest, dirname, basename in [('screenshot.jpg', '/', 'screenshot.jpg'),
                                    ('screenshot.jpg@/assets/screenshot.jpg', '/assets', 'screenshot.jpg')]:
      self.btest_exit('test_sdl2_image.c', 600, cflags=[
        '-O2',
        '--preload-file', dest,
        '-DSCREENSHOT_DIRNAME="' + dirname + '"',
        '-DSCREENSHOT_BASENAME="' + basename + '"',
        '-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '--use-preload-plugins',
      ])

  @requires_graphics_hardware
  def test_sdl2_image_jpeg(self):
    copy_asset('browser/screenshot.jpg', 'screenshot.jpeg')
    self.btest_exit('test_sdl2_image.c', 600, cflags=[
      '--preload-file', 'screenshot.jpeg',
      '-DSCREENSHOT_DIRNAME="/"', '-DSCREENSHOT_BASENAME="screenshot.jpeg"',
      '-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '--use-preload-plugins',
    ])

  @also_with_wasmfs
  @requires_graphics_hardware
  @with_all_sjlj
  @requires_safari_version(170601, 'TODO: Test enables Wasm exceptions') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), passes in Safari 18.5 (20621.2.5.11.8) and Safari 26.0.1 (21622.1.22.11.15)
  def test_sdl2_image_formats(self):
    copy_asset('browser/screenshot.png')
    copy_asset('browser/screenshot.jpg')
    self.btest_exit('test_sdl2_image.c', 512, cflags=[
      '--preload-file', 'screenshot.png',
      '-DSCREENSHOT_DIRNAME="/"', '-DSCREENSHOT_BASENAME="screenshot.png"', '-DNO_PRELOADED',
      '-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '-sSDL2_IMAGE_FORMATS=png',
    ])
    self.btest_exit('test_sdl2_image.c', 600, cflags=[
      '--preload-file', 'screenshot.jpg',
      '-DSCREENSHOT_DIRNAME="/"', '-DSCREENSHOT_BASENAME="screenshot.jpg"', '-DBITSPERPIXEL=24', '-DNO_PRELOADED',
      '--use-port=sdl2', '--use-port=sdl2_image:formats=jpg',
    ])

  def test_sdl2_key(self):
    self.btest_exit('test_sdl2_key.c', 37182145, cflags=['-sUSE_SDL=2', '--pre-js', test_file('browser/fake_events.js')])

  def test_sdl2_text(self):
    create_file('pre.js', '''
      Module.postRun = () => {
        function doOne() {
          Module._one();
          setTimeout(doOne, 1000/60);
        }
        setTimeout(doOne, 1000/60);
      }
    ''')

    self.btest_exit('test_sdl2_text.c', cflags=['--pre-js', 'pre.js', '--pre-js', test_file('browser/fake_events.js'), '-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_mouse(self):
    self.btest_exit('test_sdl2_mouse.c', cflags=['-O2', '--minify=0', '--pre-js', test_file('browser/fake_events.js'), '-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_mouse_offsets(self):
    create_file('page.html', '''
      <html>
        <head>
          <style type="text/css">
            html, body { margin: 0; padding: 0; }
            #container {
              position: absolute;
              left: 5px; right: 0;
              top: 5px; bottom: 0;
            }
            #canvas {
              position: absolute;
              left: 0; width: 600px;
              top: 0; height: 450px;
            }
            textarea {
              margin-top: 500px;
              margin-left: 5px;
              width: 600px;
            }
          </style>
        </head>
        <body>
          <div id="container">
            <canvas id="canvas"></canvas>
          </div>
          <textarea id="output" rows="8"></textarea>
          <script type="text/javascript">
            var Module = {
              canvas: document.getElementById('canvas'),
              print: (function() {
                var element = document.getElementById('output');
                element.value = ''; // clear browser cache
                return function(text) {
                  if (arguments.length > 1) text = Array.prototype.slice.call(arguments).join(' ');
                  element.value += text + "\\n";
                  element.scrollTop = element.scrollHeight; // focus on bottom
                };
              })()
            };
          </script>
          <script type="text/javascript" src="sdl2_mouse.js"></script>
        </body>
      </html>
    ''')

    self.compile_btest('browser/test_sdl2_mouse.c', ['-DTEST_SDL_MOUSE_OFFSETS', '-O2', '--minify=0', '-o', 'sdl2_mouse.js', '--pre-js', test_file('browser/fake_events.js'), '-sUSE_SDL=2', '-sEXIT_RUNTIME'])
    self.run_browser('page.html', '', '/report_result?exit:0')

  def test_sdl2_threads(self):
    self.btest_exit('test_sdl2_threads.c', cflags=['-pthread', '-sUSE_SDL=2', '-sPROXY_TO_PTHREAD'])

  @requires_graphics_hardware
  def test_sdl2_glshader(self):
    self.cflags += ['--closure=1', '-g1']
    self.reftest('test_sdl2_glshader.c', 'test_sdl_glshader.png', cflags=['-sUSE_SDL=2', '-sLEGACY_GL_EMULATION'])

  @requires_graphics_hardware
  def test_sdl2_canvas_blank(self):
    self.reftest('test_sdl2_canvas_blank.c', 'test_sdl_canvas_blank.png', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_canvas_palette(self):
    self.reftest('test_sdl2_canvas_palette.c', 'test_sdl_canvas_palette.png', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_canvas_twice(self):
    self.reftest('test_sdl2_canvas_twice.c', 'test_sdl_canvas_twice.png', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_gfx(self):
    self.reftest('test_sdl2_gfx.c', cflags=['-sUSE_SDL=2', '-sUSE_SDL_GFX=2'], reference_slack=2)

  @requires_graphics_hardware
  def test_sdl2_canvas_palette_2(self):
    create_file('args-r.js', '''
      Module['arguments'] = ['-r'];
    ''')

    create_file('args-g.js', '''
      Module['arguments'] = ['-g'];
    ''')

    create_file('args-b.js', '''
      Module['arguments'] = ['-b'];
    ''')

    self.reftest('test_sdl2_canvas_palette_2.c', 'test_sdl_canvas_palette_r.png', cflags=['-sUSE_SDL=2', '--pre-js', 'args-r.js'])
    self.reftest('test_sdl2_canvas_palette_2.c', 'test_sdl_canvas_palette_g.png', cflags=['-sUSE_SDL=2', '--pre-js', 'args-g.js'])
    self.reftest('test_sdl2_canvas_palette_2.c', 'test_sdl_canvas_palette_b.png', cflags=['-sUSE_SDL=2', '--pre-js', 'args-b.js'])

  def test_sdl2_swsurface(self):
    self.btest_exit('test_sdl2_swsurface.c', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_image_prepare(self):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.reftest('test_sdl2_image_prepare.c', 'screenshot.jpg', cflags=['--preload-file', 'screenshot.not', '-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2'])

  @requires_graphics_hardware
  def test_sdl2_image_prepare_data(self):
    # load an image file, get pixel data.
    copy_asset('browser/screenshot.jpg', 'screenshot.not')
    self.reftest('test_sdl2_image_prepare_data.c', 'screenshot.jpg', cflags=['--preload-file', 'screenshot.not', '-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2'])

  def test_sdl2_pumpevents(self):
    # key events should be detected using SDL_PumpEvents
    self.btest_exit('test_sdl2_pumpevents.c', cflags=['--pre-js', test_file('browser/fake_events.js'), '-sUSE_SDL=2'])

  @also_with_proxy_to_pthread
  def test_sdl_timer(self):
    self.btest_exit('test_sdl_timer.c', cflags=['-sUSE_SDL'])

  @also_with_proxy_to_pthread
  def test_sdl2_timer(self):
    self.btest_exit('test_sdl_timer.c', cflags=['-sUSE_SDL=2', '-DUSE_SDL2'])

  def test_sdl2_canvas_size(self):
    self.btest_exit('test_sdl2_canvas_size.c', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_gl_read(self):
    # SDL, OpenGL, readPixels
    self.btest_exit('test_sdl2_gl_read.c', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_glmatrixmode_texture(self):
    self.reftest('test_sdl2_glmatrixmode_texture.c', cflags=['-sLEGACY_GL_EMULATION', '-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_gldrawelements(self):
    self.reftest('test_sdl2_gldrawelements.c', cflags=['-sLEGACY_GL_EMULATION', '-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_glclipplane_gllighting(self):
    self.reftest('test_sdl2_glclipplane_gllighting.c', cflags=['-sLEGACY_GL_EMULATION', '-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_glalphatest(self):
    self.reftest('test_sdl2_glalphatest.c', cflags=['-sLEGACY_GL_EMULATION', '-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_fog_simple(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl2_fog_simple.c', 'screenshot-fog-simple.png',
                 cflags=['-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '-O2', '--minify=0', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins'])

  @requires_graphics_hardware
  def test_sdl2_fog_negative(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl2_fog_negative.c', 'screenshot-fog-negative.png',
                 cflags=['-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins'])

  @requires_graphics_hardware
  def test_sdl2_fog_density(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl2_fog_density.c', 'screenshot-fog-density.png',
                 cflags=['-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins'])

  @requires_graphics_hardware
  def test_sdl2_fog_exp2(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl2_fog_exp2.c', 'screenshot-fog-exp2.png',
                 cflags=['-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins'])

  @requires_graphics_hardware
  def test_sdl2_fog_linear(self):
    copy_asset('browser/screenshot.png')
    self.reftest('test_sdl2_fog_linear.c', 'screenshot-fog-linear.png', reference_slack=1,
                 cflags=['-sUSE_SDL=2', '-sUSE_SDL_IMAGE=2', '--preload-file', 'screenshot.png', '-sLEGACY_GL_EMULATION', '--use-preload-plugins'])

  def test_sdl2_unwasteful(self):
    self.btest_exit('test_sdl2_unwasteful.c', cflags=['-sUSE_SDL=2', '-O1'])

  def test_sdl2_canvas_write(self):
    self.btest_exit('test_sdl2_canvas_write.c', cflags=['-sUSE_SDL=2'])

  @requires_graphics_hardware
  def test_sdl2_ttf(self):
    copy_asset('freetype/LiberationSansBold.ttf')
    self.reftest('test_sdl2_ttf.c', cflags=['-O2', '-sUSE_SDL=2', '-sUSE_SDL_TTF=2', '--embed-file', 'LiberationSansBold.ttf'])

  @requires_graphics_hardware
  def test_sdl3_ttf(self):
    shutil.copy2(test_file('freetype/LiberationSansBold.ttf'), self.get_dir())
    self.reftest('test_sdl3_ttf.c', 'test_sdl3_ttf.png',
                 cflags=['-O2', '-sUSE_SDL=3', '-sUSE_SDL_TTF=3', '--embed-file', 'LiberationSansBold.ttf', '-Wno-experimental'])

  @requires_graphics_hardware
  def test_sdl2_ttf_rtl(self):
    copy_asset('third_party/notofont/NotoNaskhArabic-Regular.ttf')
    self.reftest('test_sdl2_ttf_rtl.c', cflags=['-O2', '-sUSE_SDL=2', '-sUSE_SDL_TTF=2', '--embed-file', 'NotoNaskhArabic-Regular.ttf'])

  def test_sdl2_custom_cursor(self):
    copy_asset('cursor.bmp')
    self.btest_exit('test_sdl2_custom_cursor.c', cflags=['--preload-file', 'cursor.bmp', '-sUSE_SDL=2'])

  def test_sdl2_misc(self):
    self.btest_exit('test_sdl2_misc.c', cflags=['-sUSE_SDL=2'])

  def test_sdl2_misc_main_module(self):
    self.btest_exit('test_sdl2_misc.c', cflags=['-sUSE_SDL=2', '-sMAIN_MODULE'])

  def test_sdl2_misc_via_object(self):
    self.emcc('browser/test_sdl2_misc.c', ['-c', '-sUSE_SDL=2', '-o', 'test.o'])
    self.compile_btest('test.o', ['-sEXIT_RUNTIME', '-sUSE_SDL=2', '-o', 'test.html'])
    self.run_browser('test.html', '/report_result?exit:0')

  @parameterized({
    '': (['-sUSE_SDL=2', '-sUSE_SDL_MIXER=2'],),
    'dash_l': (['-lSDL2', '-lSDL2_mixer'],),
  })
  @requires_sound_hardware
  def test_sdl2_mixer_wav(self, flags):
    copy_asset('sounds/the_entertainer.wav', 'sound.wav')
    self.btest_exit('test_sdl2_mixer_wav.c', cflags=['--preload-file', 'sound.wav'] + flags)

  @parameterized({
    'wav': ([],         '0',            'the_entertainer.wav'),
    'ogg': (['ogg'],    'MIX_INIT_OGG', 'alarmvictory_1.ogg'),
    'mp3': (['mp3'],    'MIX_INIT_MP3', 'pudinha.mp3'),
    'mod': (['mod'],    'MIX_INIT_MOD', 'bleep.xm'),
    # TODO: need to source freepats.cfg and a midi file
    # 'mod': (['mid'],    'MIX_INIT_MID', 'midi.mid'),
  })
  @requires_sound_hardware
  def test_sdl2_mixer_music(self, formats, flags, music_name):
    copy_asset(f'sounds/{music_name}')
    args = [
      '--preload-file', music_name,
      '-DSOUND_PATH="%s"' % music_name,
      '-DFLAGS=' + flags,
      '-sUSE_SDL=2',
      '-sUSE_SDL_MIXER=2',
      '-sSDL2_MIXER_FORMATS=' + ','.join(formats),
    ]
    # libmodplug is written in C++ so we need to link in C++
    # libraries when using it.
    if 'mod' in formats:
      args += ['-lc++', '-lc++abi']
    self.btest_exit('test_sdl2_mixer_music.c', cflags=args)

  def test_sdl3_misc(self):
    self.cflags.append('-Wno-experimental')
    self.btest_exit('test_sdl3_misc.c', cflags=['-sUSE_SDL=3'])

  def test_sdl3_canvas_write(self):
    self.cflags.append('-Wno-experimental')
    self.btest_exit('test_sdl3_canvas_write.c', cflags=['-sUSE_SDL=3'])

  @requires_graphics_hardware
  @no_wasm64('cocos2d ports does not compile with wasm64')
  def test_cocos2d_hello(self):
    # cocos2d build contains a bunch of warnings about tiff symbols being missing at link time:
    # e.g. warning: undefined symbol: TIFFClientOpen
    cocos2d_root = os.path.join(ports.Ports.get_dir(), 'cocos2d', 'Cocos2d-version_3_3r1')
    preload_file = os.path.join(cocos2d_root, 'samples', 'Cpp', 'HelloCpp', 'Resources') + '@'
    self.reftest('cocos2d_hello.cpp', reference_slack=1,
                 cflags=['-sUSE_COCOS2D=3', '-sERROR_ON_UNDEFINED_SYMBOLS=0',
                            # This line should really just be `-std=c++14` like we use to compile
                            # the cocos library itself, but that doesn't work in this case because
                            # btest adds browser_reporting.c to the command.
                            '-D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION',
                            '-Wno-js-compiler',
                            '-Wno-experimental',
                            '--preload-file', preload_file, '--use-preload-plugins',
                            '-Wno-inconsistent-missing-override',
                            '-Wno-deprecated-declarations'])

  @parameterized({
    'O0': ('-O0',),
    'O1': ('-O1',),
    'O2': ('-O2',),
    'O3': ('-O3',),
  })
  @parameterized({
    'asyncify': (['-sASYNCIFY'],),
    'asyncify_minimal_runtime': (['-sMINIMAL_RUNTIME', '-sASYNCIFY'],),
    'jspi': (['-sJSPI', '-Wno-experimental'],),
    'jspi_wasm_bigint': (['-sJSPI', '-sWASM_BIGINT', '-Wno-experimental'],),
    'jspi_wasm_bigint_minimal_runtime': (['-sMINIMAL_RUNTIME', '-sJSPI', '-sWASM_BIGINT', '-Wno-experimental'],),
  })
  def test_async(self, opt, args):
    if is_jspi(args) and not is_chrome():
      self.skipTest(f'Current browser ({get_browser()}) does not support JSPI. Only chromium-based browsers ({CHROMIUM_BASED_BROWSERS}) support JSPI today.')

    self.btest_exit('test_async.c', cflags=[opt, '-g2'] + args)

  def test_asyncify_tricky_function_sig(self):
    self.btest('test_asyncify_tricky_function_sig.cpp', '85', cflags=['-sASYNCIFY_ONLY=[foo(char.const*?.int#),foo2(),main,__original_main]', '-sASYNCIFY'])

  def test_async_in_pthread(self):
    self.btest_exit('test_async.c', cflags=['-sASYNCIFY', '-pthread', '-sPROXY_TO_PTHREAD', '-g'])

  def test_async_2(self):
    # Error.stackTraceLimit default to 10 in chrome but this test relies on more
    # than 40 stack frames being reported.
    create_file('pre.js', 'Error.stackTraceLimit = 80;\n')
    self.btest_exit('test_async_2.c', cflags=['-O3', '--pre-js', 'pre.js', '-sASYNCIFY', '-sSTACK_SIZE=1MB'])

  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
  })
  def test_async_virtual(self, args):
    self.btest_exit('async_virtual.cpp', cflags=args + ['-profiling', '-sASYNCIFY'])

  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
  })
  def test_async_virtual_2(self, args):
    self.btest_exit('async_virtual_2.cpp', cflags=args + ['-sASSERTIONS', '-sSAFE_HEAP', '-profiling', '-sASYNCIFY'])

  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
  })
  def test_async_mainloop(self, args):
    self.btest_exit('test_async_mainloop.c', cflags=args + ['-sASYNCIFY'])

  @requires_sound_hardware
  @parameterized({
    '': ([],),
    'safeheap': (['-sSAFE_HEAP'],),
  })
  def test_sdl_audio_beep_sleep(self, args):
    self.btest_exit('test_sdl_audio_beep_sleep.cpp', cflags=['-Os', '-sASSERTIONS', '-sDISABLE_EXCEPTION_CATCHING=0', '-profiling', '-lSDL', '-sASYNCIFY'] + args, timeout=90)

  def test_mainloop_reschedule(self):
    self.btest('test_mainloop_reschedule.c', '1', cflags=['-Os', '-sASYNCIFY'])

  def test_mainloop_infloop(self):
    self.btest('test_mainloop_infloop.c', '1', cflags=['-sASYNCIFY'])

  def test_async_iostream(self):
    self.btest('async_iostream.cpp', '1', cflags=['-sASYNCIFY'])

  # Test an async return value. The value goes through a custom JS library
  # method that uses asyncify, and therefore it needs to be declared in
  # ASYNCIFY_IMPORTS.
  # To make the test more precise we also use ASYNCIFY_IGNORE_INDIRECT here.
  @parameterized({
    '': (['-sASYNCIFY_IMPORTS=sync_tunnel,sync_tunnel_bool'],), # noqa
    'pattern_imports': (['-sASYNCIFY_IMPORTS=[sync_tun*]'],), # noqa
    'response': (['-sASYNCIFY_IMPORTS=@filey.txt'],), # noqa
    'nothing': (['-DBAD'],), # noqa
    'empty_list': (['-DBAD', '-sASYNCIFY_IMPORTS=[]'],), # noqa
    'em_js_bad': (['-DBAD', '-DUSE_EM_JS'],), # noqa
  })
  def test_async_returnvalue(self, args):
    if '@' in str(args):
      create_file('filey.txt', 'sync_tunnel\nsync_tunnel_bool\n')
    self.btest('test_async_returnvalue.c', '0', cflags=['-sASSERTIONS', '-sASYNCIFY', '-sASYNCIFY_IGNORE_INDIRECT', '--js-library', test_file('browser/test_async_returnvalue.js')] + args)

  @no_safari('TODO: Never reports a result, so times out') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_async_bad_list(self):
    self.btest('test_async_bad_list.c', '0', cflags=['-sASYNCIFY', '-sASYNCIFY_ONLY=waka', '--profiling'])

  # Tests that when building with -sMINIMAL_RUNTIME, the build can use -sMODULARIZE as well.
  def test_minimal_runtime_modularize(self):
    self.btest_exit('browser_test_hello_world.c', cflags=['-sMODULARIZE', '-sMINIMAL_RUNTIME'])

  # Tests that when building with -sMINIMAL_RUNTIME, the build can use -sEXPORT_NAME=Foo as well.
  def test_minimal_runtime_export_name(self):
    self.btest_exit('browser_test_hello_world.c', cflags=['-sEXPORT_NAME=Foo', '-sMINIMAL_RUNTIME'])

  @parameterized({
    # defaults
    '': ([], '''
       let promise = Module();
       if (!promise instanceof Promise) throw new Error('Return value should be a promise');
    '''),
    # use EXPORT_NAME
    'export_name': (['-sEXPORT_NAME="HelloWorld"'], '''
       if (typeof Module !== "undefined") throw "what?!"; // do not pollute the global scope, we are modularized!
       HelloWorld.noInitialRun = true; // erroneous module capture will load this and cause timeout
       let promise = HelloWorld();
       if (!promise instanceof Promise) throw new Error('Return value should be a promise');
    '''),
    # pass in a Module option (which prevents main(), which we then invoke ourselves)
    'no_main': (['-sEXPORT_NAME="HelloWorld"'], '''
       HelloWorld({ noInitialRun: true }).then(hello => {
         hello._main();
       });
    '''),
  })
  @parameterized({
    '': ([],),
    'O1': (['-O1'],),
    'O2': (['-O2'],),
    'profiling': (['-O2', '-profiling'],),
    'closure': (['-O2', '--closure=1'],),
  })
  def test_modularize(self, args, code, opts):
    # this test is synchronous, so avoid async startup due to wasm features
    self.compile_btest('browser_test_hello_world.c', ['-sMODULARIZE', '-sSINGLE_FILE'] + args + opts)
    create_file('a.html', '''
      <!DOCTYPE html><html lang="en"><head><meta charset="utf-8"></head><body>
      <script src="a.out.js"></script>
      <script>
        %s
      </script>
      </body></html>
    ''' % code)
    self.run_browser('a.html', '/report_result?0')

  @no_firefox('source phase imports not implemented yet in firefox')
  @no_safari('TODO: croaks on line "import source wasmModule from \'./out.wasm\';"') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_source_phase_imports(self):
    self.compile_btest('browser_test_hello_world.c', ['-sEXPORT_ES6', '-sSOURCE_PHASE_IMPORTS', '-Wno-experimental', '-o', 'out.mjs'])
    create_file('a.html', '''
      <script type="module">
        import Module from "./out.mjs"
        const mod = await Module();
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0')

  def test_modularize_network_error(self):
    self.compile_btest('browser_test_hello_world.c', ['-sMODULARIZE', '-sEXPORT_NAME=createModule'], reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', '''
      <script src="browser_reporting.js"></script>
      <script src="a.out.js"></script>
      <script>
        createModule()
          .then(() => {
            reportResultToServer("Module creation succeeded when it should have failed");
          })
          .catch(err => {
            reportResultToServer(err.message);
          });
      </script>
    ''')
    print('Deleting a.out.wasm to cause a download error')
    os.remove('a.out.wasm')
    self.run_browser('a.html', '/report_result?Aborted(both async and sync fetching of the wasm failed)')

  def test_modularize_init_error(self):
    self.compile_btest('browser/test_modularize_init_error.cpp', ['-sMODULARIZE', '-sEXPORT_NAME=createModule'], reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', '''
      <script src="browser_reporting.js"></script>
      <script src="a.out.js"></script>
      <script>
        createModule()
          .then(() => {
            reportResultToServer("Module creation succeeded when it should have failed");
          })
          .catch(err => {
            reportResultToServer(err);
          });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?intentional error to test rejection')

  # test illustrating the regression on the modularize feature since commit c5af8f6
  # when compiling with the --preload-file option
  @requires_wasm2js
  @also_with_wasmfs
  @parameterized({
    '': ([],),
    'O1': (['-O1'],),
    'O2_profiling': (['-O2', '--profiling'],),
    'O2_closure': (['-O2', '--closure=1'],),
  })
  def test_modularize_and_preload_files(self, args):
    self.set_setting('EXIT_RUNTIME')
    # TODO(sbc): Fix closure warnings with MODULARIZE + WASM=0
    self.ldflags.append('-Wno-error=closure')
    # amount of memory different from the default one that will be allocated for the emscripten heap
    totalMemory = 33554432

    # the main function simply checks that the amount of allocated heap memory is correct
    create_file('test.c', r'''
      #include <stdio.h>
      #include <emscripten.h>
      int main() {
        EM_ASM({
          // use eval here in order for the test with closure compiler enabled to succeed
          var totalMemory = Module['INITIAL_MEMORY'];
          assert(totalMemory === %d, 'bad memory size');
        });
        return 0;
      }
    ''' % totalMemory)
    # generate a dummy file
    create_file('dummy_file', 'dummy')
    # compile the code with the modularize feature and the preload-file option enabled
    # no wasm, since this tests customizing total memory at runtime
    self.compile_btest('test.c', ['-sWASM=0', '-sIMPORTED_MEMORY', '-sMODULARIZE', '-sEXPORT_NAME=Foo', '--preload-file', 'dummy_file'] + args, reporting=Reporting.JS_ONLY)
    create_file('a.html', '''
      <script src="a.out.js"></script>
      <script>
        // instantiate the Foo module with custom INITIAL_MEMORY value
        var foo = Foo({ INITIAL_MEMORY: %d });
      </script>
    ''' % totalMemory)
    self.run_browser('a.html', '/report_result?exit:0')

  @parameterized({
    '': ([],),
    'O1': (['-O1'],),
    'O2': (['-O2'],),
  })
  def test_webidl(self, args):
    # see original in test_core.py
    self.run_process([WEBIDL_BINDER, test_file('webidl/test.idl'), 'glue'])
    self.assertExists('glue.cpp')
    self.assertExists('glue.js')
    self.btest('webidl/test.cpp', '1', cflags=['--post-js', 'glue.js', '-I.', '-DBROWSER'] + args)

  @no_wasm64('https://github.com/llvm/llvm-project/issues/98778')
  def test_dylink(self):
    create_file('main.c', r'''
      #include <assert.h>
      #include <stdio.h>
      #include <stdlib.h>
      #include <string.h>
      char *side(const char *data);
      int main() {
        char *ret = side("hello through side\n");
        puts(ret);
        assert(strcmp(ret, "hello through side\n") == 0);
        return 0;
      }
    ''')
    create_file('side.c', r'''
      #include <string.h>
      char *side(const char *data) {
        return strdup(data);
      }
    ''')
    self.emcc('side.c', ['-sSIDE_MODULE', '-O2', '-o', 'side.wasm'])
    self.btest_exit('main.c', cflags=['-sMAIN_MODULE=2', '-O2', 'side.wasm'])

  def test_dlopen_async(self):
    create_file('side.c', 'int foo = 42;\n')
    self.emcc('side.c', ['-o', 'libside.so', '-sSIDE_MODULE'])
    self.btest_exit('other/test_dlopen_async.c', cflags=['-sMAIN_MODULE=2'])

  @requires_shared_array_buffer
  def test_dlopen_blocking(self):
    self.emcc('other/test_dlopen_blocking_side.c', ['-o', 'libside.so', '-sSIDE_MODULE', '-pthread', '-Wno-experimental'])
    # Attempt to use dlopen the side module (without preloading) should fail on the main thread
    # since the synchronous `readBinary` function does not exist.
    self.btest_exit('other/test_dlopen_blocking.c', assert_returncode=1, cflags=['-sMAIN_MODULE=2', '-sAUTOLOAD_DYLIBS=0', 'libside.so'])
    # But with PROXY_TO_PTHEAD it does work, since we can do blocking and sync XHR in a worker.
    self.btest_exit('other/test_dlopen_blocking.c', cflags=['-sMAIN_MODULE=2', '-sPROXY_TO_PTHREAD', '-pthread', '-Wno-experimental', '-sAUTOLOAD_DYLIBS=0', 'libside.so'])

  # verify that dynamic linking works in all kinds of in-browser environments.
  # don't mix different kinds in a single test.
  @parameterized({
    '': (False,),
    'inworker': (True,),
  })
  def test_dylink_dso_needed(self, inworker):
    if not inworker:
      self.skipTest('https://github.com/emscripten-core/emscripten/issues/25814')

    def do_run(src, expected_output, cflags):
      # XXX there is no infrastructure (yet ?) to retrieve stdout from browser in tests.
      # -> do the assert about expected output inside browser.
      #
      # we have to put the hook into post.js because in main it is too late
      # (in main we won't be able to catch what static constructors inside
      # linked dynlibs printed), and in pre.js it is too early (out is not yet
      # setup by the shell).
      create_file('post.js', r'''
          Module.realPrint = out;
          out = (x) => {
            if (!Module.printed) Module.printed = "";
            Module.printed += x + '\n'; // out is passed str without last \n
            Module.realPrint(x);
          };
        ''')
      create_file('test_dylink_dso_needed.c', src + r'''
        #include <emscripten/em_asm.h>

        int main() {
          int rtn = test_main();
          EM_ASM({
            var expected = %r;
            assert(Module.printed === expected, ['stdout expected:', expected]);
          });
          return rtn;
        }
      ''' % expected_output)
      self.btest_exit('test_dylink_dso_needed.c', cflags=['--post-js', 'post.js'] + cflags, run_in_worker=inworker)

    self._test_dylink_dso_needed(do_run)

  @requires_graphics_hardware
  @no_wasm64('https://github.com/llvm/llvm-project/issues/98778')
  def test_dylink_glemu(self):
    create_file('main.c', r'''
      #include <stdio.h>
      #include <string.h>
      #include <assert.h>
      const char *side();
      int main() {
        const char *exts = side();
        puts(side());
        assert(strstr(exts, "GL_EXT_texture_env_combine"));
        return 0;
      }
    ''')
    create_file('side.c', r'''
      #include "SDL/SDL.h"
      #include "SDL/SDL_opengl.h"
      const char *side() {
        SDL_Init(SDL_INIT_VIDEO);
        SDL_SetVideoMode(600, 600, 16, SDL_OPENGL);
        return (const char *)glGetString(GL_EXTENSIONS);
      }
    ''')
    self.emcc('side.c', ['-sSIDE_MODULE', '-O2', '-o', 'side.wasm', '-lSDL'])

    self.btest_exit('main.c', cflags=['-sMAIN_MODULE=2', '-O2', '-sLEGACY_GL_EMULATION', '-lSDL', '-lGL', 'side.wasm'])

  def test_dylink_many(self):
    # test asynchronously loading two side modules during startup
    create_file('main.c', r'''
      #include <assert.h>
      int side1();
      int side2();
      int main() {
        assert(side1() == 1);
        assert(side2() == 2);
        return 0;
      }
    ''')
    create_file('side1.c', r'''
      int side1() { return 1; }
    ''')
    create_file('side2.c', r'''
      int side2() { return 2; }
    ''')
    self.emcc('side1.c', ['-sSIDE_MODULE', '-o', 'side1.wasm'])
    self.emcc('side2.c', ['-sSIDE_MODULE', '-o', 'side2.wasm'])
    self.btest_exit('main.c', cflags=['-sMAIN_MODULE=2', 'side1.wasm', 'side2.wasm'])

  def test_dylink_pthread_many(self):
    # Test asynchronously loading two side modules during startup
    # They should always load in the same order
    # Verify that function pointers in the browser's main thread
    # refer to the same function as in a pthread worker.

    # The main thread function table is populated asynchronously
    # in the browser's main thread. However, it should still be
    # populated in the same order as in a pthread worker to
    # guarantee function pointer interop.
    create_file('main.cpp', r'''
      #include <cassert>
      #include <thread>
      #include <emscripten/emscripten.h>
      int side1();
      int side2();
      int main() {
        auto side1_ptr = &side1;
        auto side2_ptr = &side2;
        // Don't join the thread since this is running in the
        // browser's main thread.
        std::thread([=]{
          assert(side1_ptr == &side1);
          assert(side2_ptr == &side2);
          emscripten_force_exit(0);
        }).detach();
        emscripten_exit_with_live_runtime();
      }
    ''')

    # The browser will try to load side1 first.
    # Use a big payload in side1 so that it takes longer to load than side2
    create_file('side1.cpp', r'''
      char const * payload1 = "''' + str(list(range(1, int(1e5)))) + r'''";
      int side1() { return 1; }
    ''')
    create_file('side2.cpp', r'''
      char const * payload2 = "0";
      int side2() { return 2; }
    ''')
    self.emcc('side1.cpp', ['-Wno-experimental', '-pthread', '-sSIDE_MODULE', '-o', 'side1.wasm'])
    self.emcc('side2.cpp', ['-Wno-experimental', '-pthread', '-sSIDE_MODULE', '-o', 'side2.wasm'])
    self.btest_exit('main.cpp',
                    cflags=['-Wno-experimental', '-pthread', '-sMAIN_MODULE=2', 'side1.wasm', 'side2.wasm'])

  @no_2gb('uses INITIAL_MEMORY')
  @no_4gb('uses INITIAL_MEMORY')
  def test_memory_growth_during_startup(self):
    create_file('data.dat', 'X' * (30 * 1024 * 1024))
    self.btest_exit('browser_test_hello_world.c', cflags=['-sASSERTIONS', '-sALLOW_MEMORY_GROWTH', '-sINITIAL_MEMORY=16MB', '-sSTACK_SIZE=16384', '--preload-file', 'data.dat'])

  # pthreads tests

  def prep_no_SAB(self):
    create_file('html.html', read_file(path_from_root('html/shell_minimal.html')).replace('''<body>''', '''<body>
      <script>
        SharedArrayBuffer = undefined;
        Atomics = undefined;
      </script>
    '''))

  def test_pthread_c11_threads(self):
    self.btest_exit('pthread/test_pthread_c11_threads.c', cflags=['-gsource-map', '-pthread', '-sPROXY_TO_PTHREAD'])

  def test_pthread_pool_size_strict(self):
    # Check that it doesn't fail with sufficient number of threads in the pool.
    self.btest_exit('pthread/test_pthread_c11_threads.c', cflags=['-g2', '-pthread', '-sPTHREAD_POOL_SIZE=4', '-sPTHREAD_POOL_SIZE_STRICT=2'])
    # Check that it fails instead of deadlocking on insufficient number of threads in the pool.
    self.btest('pthread/test_pthread_c11_threads.c',
               expected='abort:Assertion failed: thrd_create(&t4, thread_main, NULL) == thrd_success',
               cflags=['-g2', '-pthread', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2'])

  def test_pthread_in_pthread_pool_size_strict(self):
    # Check that it fails when there's a pthread creating another pthread.
    self.btest_exit('pthread/test_pthread_create_pthread.c', cflags=['-g2', '-pthread', '-sPTHREAD_POOL_SIZE=2', '-sPTHREAD_POOL_SIZE_STRICT=2'])
    # Check that it fails when there's a pthread creating another pthread.
    self.btest_exit('pthread/test_pthread_create_pthread.c', cflags=['-g2', '-pthread', '-sPTHREAD_POOL_SIZE=1', '-sPTHREAD_POOL_SIZE_STRICT=2', '-DSMALL_POOL'])

  # Test that the emscripten_ atomics api functions work.
  @parameterized({
    '': ([],),
    'closure': (['--closure=1'],),
  })
  def test_pthread_atomics(self, args):
    self.btest_exit('pthread/test_pthread_atomics.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8', '-g1'] + args)

  # Test 64-bit atomics.
  def test_pthread_64bit_atomics(self):
    self.btest_exit('pthread/test_pthread_64bit_atomics.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test 64-bit C++11 atomics.
  @also_with_threads
  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
  })
  def test_pthread_64bit_cxx11_atomics(self, opt):
    self.btest_exit('pthread/test_pthread_64bit_cxx11_atomics.cpp', cflags=opt)

  # Test c++ std::thread::hardware_concurrency()
  def test_pthread_hardware_concurrency(self):
    self.btest_exit('pthread/test_pthread_hardware_concurrency.cpp', cflags=['-O2', '-pthread', '-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency'])

  # Test that we error if not ALLOW_BLOCKING_ON_MAIN_THREAD
  def test_pthread_main_thread_blocking_wait(self):
    self.btest('pthread/main_thread_wait.c', expected='abort:Blocking on the main thread is not allowed by default.', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE', '-sALLOW_BLOCKING_ON_MAIN_THREAD=0'])

  # Test that we error or warn depending on ALLOW_BLOCKING_ON_MAIN_THREAD or ASSERTIONS
  def test_pthread_main_thread_blocking_join(self):
    create_file('pre.js', '''
      Module['printErr'] = (x) => {
        if (x.includes('Blocking on the main thread is very dangerous')) {
          maybeReportResultToServer('got_warn');
        }
      };
    ''')
    # Test that we warn about blocking on the main thread in debug builds
    self.btest('pthread/main_thread_join.cpp', expected='got_warn', cflags=['-sEXIT_RUNTIME', '-sASSERTIONS', '--pre-js', 'pre.js', '-pthread', '-sPTHREAD_POOL_SIZE'])
    # Test that we do not warn about blocking on the main thread in release builds
    self.btest_exit('pthread/main_thread_join.cpp', cflags=['-O3', '--pre-js', 'pre.js', '-pthread', '-sPTHREAD_POOL_SIZE'])
    # Test that tryjoin is fine, even if not ALLOW_BLOCKING_ON_MAIN_THREAD
    self.btest_exit('pthread/main_thread_join.cpp', assert_returncode=2, cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE', '-g', '-DTRY_JOIN', '-sALLOW_BLOCKING_ON_MAIN_THREAD=0'])
    # Test that tryjoin is fine, even if not ALLOW_BLOCKING_ON_MAIN_THREAD, and even without a pool
    self.btest_exit('pthread/main_thread_join.cpp', assert_returncode=2, cflags=['-O3', '-pthread', '-g', '-DTRY_JOIN', '-sALLOW_BLOCKING_ON_MAIN_THREAD=0'])
    # Test that everything works ok when we are on a pthread
    self.btest_exit('pthread/main_thread_join.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE', '-sPROXY_TO_PTHREAD', '-sALLOW_BLOCKING_ON_MAIN_THREAD=0'])

  # Test the old GCC atomic __sync_fetch_and_op builtin operations.
  @parameterized({
    '': (['-g'],),
    'O1': (['-O1', '-g'],),
    'O2': (['-O2'],),
    'O3': (['-O3'],),
    'Os': (['-Os'],),
  })
  def test_pthread_gcc_atomic_fetch_and_op(self, args):
    self.cflags += ['-Wno-sync-fetch-and-nand-semantics-changed']
    self.btest_exit('pthread/test_pthread_gcc_atomic_fetch_and_op.c', cflags=args + ['-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # 64 bit version of the above test.
  @also_with_wasm2js
  def test_pthread_gcc_64bit_atomic_fetch_and_op(self):
    if self.is_wasm2js():
      self.skipTest('https://github.com/WebAssembly/binaryen/issues/4358')
    self.cflags += ['-Wno-sync-fetch-and-nand-semantics-changed']
    self.btest_exit('pthread/test_pthread_gcc_64bit_atomic_fetch_and_op.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test the old GCC atomic __sync_op_and_fetch builtin operations.
  @also_with_wasm2js
  @requires_safari_version(170601, 'TODO: browser.test_pthread_gcc_atomic_op_and_fetch_wasm2js fails with "abort:Assertion failed: nand_and_fetch_data == -1"') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), passes in Safari 18.5 (20621.2.5.11.8) and Safari 26.0.1 (21622.1.22.11.15)
  def test_pthread_gcc_atomic_op_and_fetch(self):
    self.cflags += ['-Wno-sync-fetch-and-nand-semantics-changed']
    self.btest_exit('pthread/test_pthread_gcc_atomic_op_and_fetch.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # 64 bit version of the above test.
  @also_with_wasm2js
  def test_pthread_gcc_64bit_atomic_op_and_fetch(self):
    if self.is_wasm2js():
      self.skipTest('https://github.com/WebAssembly/binaryen/issues/4358')
    self.cflags += ['-Wno-sync-fetch-and-nand-semantics-changed', '--profiling-funcs']
    self.btest_exit('pthread/test_pthread_gcc_64bit_atomic_op_and_fetch.c', cflags=['-pthread', '-O2', '-sPTHREAD_POOL_SIZE=8'])

  # Tests the rest of the remaining GCC atomics after the two above tests.
  @also_with_wasm2js
  def test_pthread_gcc_atomics(self):
    self.btest_exit('pthread/test_pthread_gcc_atomics.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test the __sync_lock_test_and_set and __sync_lock_release primitives.
  @also_with_wasm2js
  @parameterized({
    '': ([],),
    'em_instrinsics': (['-DUSE_EMSCRIPTEN_INTRINSICS'],),
  })
  def test_pthread_gcc_spinlock(self, args):
    self.btest_exit('pthread/test_pthread_gcc_spinlock.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'] + args)

  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
    'minimal_runtime': (['-sMINIMAL_RUNTIME'],),
    'single_file': (['-sSINGLE_FILE'],),
  })
  def test_pthread_create(self, args):
    self.btest_exit('pthread/test_pthread_create.c',
                    cflags=['-pthread', '-sPTHREAD_POOL_SIZE=8'] + args)
    files = os.listdir('.')
    if '-sSINGLE_FILE' in args:
      self.assertEqual(len(files), 1, files)
    else:
      self.assertEqual(len(files), 3, files)

  # Test that preallocating worker threads work.
  def test_pthread_preallocates_workers(self):
    self.btest_exit('pthread/test_pthread_preallocates_workers.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=4', '-sPTHREAD_POOL_DELAY_LOAD'])

  # Test that allocating a lot of threads doesn't regress. This needs to be checked manually!
  @no_2gb('uses INITIAL_MEMORY')
  @no_4gb('uses INITIAL_MEMORY')
  def test_pthread_large_pthread_allocation(self):
    self.btest_exit('pthread/test_large_pthread_allocation.c', cflags=['-sINITIAL_MEMORY=128MB', '-O3', '-pthread', '-sPTHREAD_POOL_SIZE=50'])

  # Tests the -sPROXY_TO_PTHREAD option.
  def test_pthread_proxy_to_pthread(self):
    self.btest_exit('pthread/test_pthread_proxy_to_pthread.c', cflags=['-O3', '-pthread', '-sPROXY_TO_PTHREAD'])

  # Test that a pthread can spawn another pthread of its own.
  @parameterized({
    '': ([],),
    'modularize': (['-sMODULARIZE'],),
  })
  def test_pthread_create_pthread(self, args):
    self.btest_exit('pthread/test_pthread_create_pthread.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=2'] + args)

  # Test another case of pthreads spawning pthreads, but this time the callers immediately join on the threads they created.
  def test_pthread_nested_spawns(self):
    self.btest_exit('pthread/test_pthread_nested_spawns.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=2'])

  # Test that main thread can wait for a pthread to finish via pthread_join().
  def test_pthread_join(self):
    self.btest_exit('pthread/test_pthread_join.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test that threads can rejoin the pool once detached and finished
  def test_std_thread_detach(self):
    self.btest_exit('pthread/test_std_thread_detach.cpp', cflags=['-pthread'])

  # Test pthread_cancel() operation
  def test_pthread_cancel(self):
    self.btest_exit('pthread/test_pthread_cancel.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test that pthread_cancel() cancels pthread_cond_wait() operation
  def test_pthread_cancel_cond_wait(self):
    self.btest_exit('pthread/test_pthread_cancel_cond_wait.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test pthread_kill() operation
  @no_chrome('pthread_kill hangs chrome renderer, and keep subsequent tests from passing')
  def test_pthread_kill(self):
    self.btest_exit('pthread/test_pthread_kill.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test that pthread cleanup stack (pthread_cleanup_push/_pop) works.
  def test_pthread_cleanup(self):
    self.btest_exit('pthread/test_pthread_cleanup.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Tests the pthread mutex api.
  @parameterized({
    '': ([],),
    'spinlock': (['-DSPINLOCK_TEST'],),
  })
  @requires_safari_version(170601, 'TODO: browser.test_pthread_mutex and browser.test_pthread_mutex_spinlock both hang Safari') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), passes in Safari 18.5 (20621.2.5.11.8) and Safari 26.0.1 (21622.1.22.11.15)
  def test_pthread_mutex(self, args):
    self.btest_exit('pthread/test_pthread_mutex.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'] + args)

  def test_pthread_attr_getstack(self):
    self.btest_exit('pthread/test_pthread_attr_getstack.c', cflags=['-pthread', '-sPTHREAD_POOL_SIZE=2'])

  # Test that memory allocation is thread-safe.
  def test_pthread_malloc(self):
    self.btest_exit('pthread/test_pthread_malloc.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Stress test pthreads allocating memory that will call to sbrk(), and main thread has to free up the data.
  def test_pthread_malloc_free(self):
    self.btest_exit('pthread/test_pthread_malloc_free.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test that the pthread_barrier API works ok.
  def test_pthread_barrier(self):
    self.btest_exit('pthread/test_pthread_barrier.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test the pthread_once() function.
  def test_pthread_once(self):
    self.btest_exit('pthread/test_pthread_once.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test against a certain thread exit time handling bug by spawning tons of threads.
  def test_pthread_spawns(self):
    self.btest_exit('pthread/test_pthread_spawns.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8', '--closure=1', '-sENVIRONMENT=web'])

  # It is common for code to flip volatile global vars for thread control. This is a bit lax, but nevertheless, test whether that
  # kind of scheme will work with Emscripten as well.
  @parameterized({
    '': (['-DUSE_C_VOLATILE'],),
    'atomic': ([],),
  })
  def test_pthread_volatile(self, args):
    self.btest_exit('pthread/test_pthread_volatile.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'] + args)

  # Test thread-specific data (TLS).
  def test_pthread_thread_local_storage(self):
    self.btest_exit('pthread/test_pthread_thread_local_storage.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8', '-sASSERTIONS'])

  # Test the pthread condition variable creation and waiting.
  def test_pthread_condition_variable(self):
    self.btest_exit('pthread/test_pthread_condition_variable.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8'])

  # Test that pthreads are able to do printf.
  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
    'debug': (['-sLIBRARY_DEBUG'],),
  })
  def test_pthread_printf(self, args):
     self.btest_exit('pthread/test_pthread_printf.c', cflags=['-pthread', '-sPTHREAD_POOL_SIZE'] + args)

  # Test that pthreads are able to do cout. Failed due to https://bugzil.la/1154858.
  def test_pthread_iostream(self):
    self.btest_exit('pthread/test_pthread_iostream.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE'])

  def test_pthread_unistd_io_bigint(self):
    self.btest_exit('unistd/io.c', cflags=['-pthread', '-sPROXY_TO_PTHREAD', '-sWASM_BIGINT'])

  # Test that the main thread is able to use pthread_set/getspecific.
  @also_with_wasm2js
  def test_pthread_setspecific_mainthread(self):
    self.btest_exit('pthread/test_pthread_setspecific_mainthread.c', cflags=['-O3', '-pthread'])

  # Test that pthreads have access to filesystem.
  def test_pthread_file_io(self):
    self.btest_exit('pthread/test_pthread_file_io.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE'])

  # Test that the pthread_create() function operates benignly in the case that threading is not supported.
  @parameterized({
   '': ([],),
   'mt': (['-pthread', '-sPTHREAD_POOL_SIZE=8'],),
  })
  def test_pthread_supported(self, args):
    self.btest_exit('pthread/test_pthread_supported.c', cflags=['-O3'] + args)

  def test_pthread_dispatch_after_exit(self):
    self.btest_exit('pthread/test_pthread_dispatch_after_exit.c', cflags=['-pthread'])

  # Test that if the main thread is performing a futex wait while a pthread
  # needs it to do a proxied operation (before that pthread would wake up the
  # main thread), that it's not a deadlock.
  def test_pthread_proxying_in_futex_wait(self):
    self.btest_exit('pthread/test_pthread_proxying_in_futex_wait.cpp', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE'])

  # Test that sbrk() operates properly in multithreaded conditions
  @no_2gb('uses INITIAL_MEMORY')
  @no_4gb('uses INITIAL_MEMORY')
  @parameterized({
    '': (['-DABORTING_MALLOC=0', '-sABORTING_MALLOC=0'],),
    'aborting_malloc': (['-DABORTING_MALLOC=1'],),
  })
  def test_pthread_sbrk(self, args):
    # With aborting malloc = 1, test allocating memory in threads
    # With aborting malloc = 0, allocate so much memory in threads that some of the allocations fail.
    self.btest_exit('pthread/test_pthread_sbrk.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE=8', '-sINITIAL_MEMORY=128MB'] + args)

  # Test that -sABORTING_MALLOC=0 works in both pthreads and non-pthreads
  # builds. (sbrk fails gracefully)
  @also_with_threads
  @parameterized({
    '': ([],),
    'O2': (['-O2'],),
  })
  def test_gauge_available_memory(self, args):
    self.btest_exit('test_gauge_available_memory.c', cflags=['-sABORTING_MALLOC=0'] + args)

  # Test that the proxying operations of user code from pthreads to main thread
  # work
  def test_pthread_run_on_main_thread(self):
    self.btest_exit('pthread/test_pthread_run_on_main_thread.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE'])

  # Test how a lot of back-to-back called proxying operations behave.
  def test_pthread_run_on_main_thread_flood(self):
    self.btest_exit('pthread/test_pthread_run_on_main_thread_flood.c', cflags=['-O3', '-pthread', '-sPTHREAD_POOL_SIZE'])

  # Test that it is possible to asynchronously call a JavaScript function on the
  # main thread.
  def test_pthread_call_async(self):
    self.btest_exit('pthread/call_async.c', cflags=['-pthread'])

  # Test that it is possible to synchronously call a JavaScript function on the
  # main thread and get a return value back.
  def test_pthread_call_sync_on_main_thread(self):
    self.btest_exit('pthread/call_sync_on_main_thread.c', cflags=['-O3', '-pthread', '-sPROXY_TO_PTHREAD', '-DPROXY_TO_PTHREAD=1', '--js-library', test_file('pthread/call_sync_on_main_thread.js')])
    self.btest_exit('pthread/call_sync_on_main_thread.c', cflags=['-O3', '-pthread', '-DPROXY_TO_PTHREAD=0', '--js-library', test_file('pthread/call_sync_on_main_thread.js')])
    self.btest_exit('pthread/call_sync_on_main_thread.c', cflags=['-Oz', '-DPROXY_TO_PTHREAD=0', '--js-library', test_file('pthread/call_sync_on_main_thread.js')])

  # Test that it is possible to asynchronously call a JavaScript function on the
  # main thread.
  def test_pthread_call_async_on_main_thread(self):
    self.btest('pthread/call_async_on_main_thread.c', expected='7', cflags=['-O3', '-pthread', '-sPROXY_TO_PTHREAD', '-DPROXY_TO_PTHREAD=1', '--js-library', test_file('pthread/call_async_on_main_thread.js')])
    self.btest('pthread/call_async_on_main_thread.c', expected='7', cflags=['-O3', '-pthread', '-DPROXY_TO_PTHREAD=0', '--js-library', test_file('pthread/call_async_on_main_thread.js')])
    self.btest('pthread/call_async_on_main_thread.c', expected='7', cflags=['-Oz', '-DPROXY_TO_PTHREAD=0', '--js-library', test_file('pthread/call_async_on_main_thread.js')])

  # Tests that spawning a new thread does not cause a reinitialization of the
  # global data section of the application memory area.
  @parameterized({
    '': (['-O3'],),
    'modularize': (['-sMODULARIZE'],),
  })
  def test_pthread_global_data_initialization(self, args):
    self.btest_exit('pthread/test_pthread_global_data_initialization.c', cflags=args + ['-pthread', '-sPROXY_TO_PTHREAD', '-sPTHREAD_POOL_SIZE'])

  @requires_wasm2js
  def test_pthread_global_data_initialization_in_sync_compilation_mode(self):
    self.btest_exit('pthread/test_pthread_global_data_initialization.c', cflags=['-sWASM_ASYNC_COMPILATION=0', '-pthread', '-sPROXY_TO_PTHREAD', '-sPTHREAD_POOL_SIZE'])

  # Test that emscripten_get_now() reports coherent wallclock times across all
  # pthreads, instead of each pthread independently reporting wallclock times
  # since the launch of that pthread.
  def test_pthread_clock_drift(self):
    self.btest_exit('pthread/test_pthread_clock_drift.c', cflags=['-O3', '-pthread', '-sPROXY_TO_PTHREAD'])

  def test_pthread_utf8_funcs(self):
    self.btest_exit('pthread/test_pthread_utf8_funcs.c', cflags=['-pthread', '-sPTHREAD_POOL_SIZE'])

  # Test the emscripten_futex_wake(addr, INT_MAX); functionality to wake all
  # waiters
  @also_with_wasm2js
  def test_pthread_wake_all(self):
    self.btest_exit('pthread/test_futex_wake_all.c', cflags=['-O3', '-pthread'])

  # Test that stack base and max correctly bound the stack on pthreads.
  def test_pthread_stack_bounds(self):
    self.btest_exit('pthread/test_pthread_stack_bounds.cpp', cflags=['-pthread'])

  # Test that real `thread_local` works.
  def test_pthread_tls(self):
    self.btest_exit('pthread/test_pthread_tls.c', cflags=['-sPROXY_TO_PTHREAD', '-pthread'])

  # Test that real `thread_local` works in main thread without PROXY_TO_PTHREAD.
  def test_pthread_tls_main(self):
    self.btest_exit('pthread/test_pthread_tls_main.cpp', cflags=['-pthread'])

  def test_pthread_safe_stack(self):
    # Note that as the test runs with PROXY_TO_PTHREAD, we set STACK_SIZE,
    # and not DEFAULT_PTHREAD_STACK_SIZE, as the pthread for main() gets the
    # same stack size as the main thread normally would.
    self.btest('core/test_safe_stack.c', expected='abort:stack overflow', cflags=['-pthread', '-sPROXY_TO_PTHREAD', '-sSTACK_OVERFLOW_CHECK=2', '-sSTACK_SIZE=64KB'])

  @no_wasm64('TODO: ASAN in memory64')
  @parameterized({
    'leak': ['test_pthread_lsan_leak', ['-gsource-map']],
    'no_leak': ['test_pthread_lsan_no_leak', []],
  })
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/15978')
  @no_safari('TODO: browser.test_pthread_lsan_leak fails with /report_result?0') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_pthread_lsan(self, name, args):
    self.btest(Path('pthread', name + '.cpp'), expected='1', cflags=['-fsanitize=leak', '-pthread', '-sPROXY_TO_PTHREAD', '--pre-js', test_file('pthread', name + '.js')] + args)

  @no_wasm64('TODO: ASAN in memory64')
  @no_2gb('ASAN + GLOBAL_BASE')
  @no_4gb('ASAN + GLOBAL_BASE')
  @parameterized({
    # Reusing the LSan test files for ASan.
    'leak': ['test_pthread_lsan_leak', ['-gsource-map']],
    'no_leak': ['test_pthread_lsan_no_leak', []],
  })
  @no_safari('TODO: browser.test_pthread_asan_leak fails with /report_result?0') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_pthread_asan(self, name, args):
    self.btest(Path('pthread', name + '.cpp'), expected='1', cflags=['-fsanitize=address', '-pthread', '-sPROXY_TO_PTHREAD', '--pre-js', test_file('pthread', name + '.js')] + args)

  @no_wasm64('TODO: ASAN in memory64')
  @no_2gb('ASAN + GLOBAL_BASE')
  @no_4gb('ASAN + GLOBAL_BASE')
  def test_pthread_asan_use_after_free(self):
    self.btest('pthread/test_pthread_asan_use_after_free.cpp', expected='1', cflags=['-fsanitize=address', '-pthread', '-sPROXY_TO_PTHREAD', '--pre-js', test_file('pthread/test_pthread_asan_use_after_free.js')])

  @no_wasm64('TODO: ASAN in memory64')
  @no_2gb('ASAN + GLOBAL_BASE')
  @no_4gb('ASAN + GLOBAL_BASE')
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/20006')
  @no_safari('TODO: Hangs') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  @also_with_wasmfs
  def test_pthread_asan_use_after_free_2(self):
    # similar to test_pthread_asan_use_after_free, but using a pool instead
    # of proxy-to-pthread, and also the allocation happens on the pthread
    # (which tests that it can use the offset converter to get the stack
    # trace there)
    self.btest('pthread/test_pthread_asan_use_after_free_2.cpp', expected='1', cflags=['-fsanitize=address', '-pthread', '-sPTHREAD_POOL_SIZE=1', '--pre-js', test_file('pthread/test_pthread_asan_use_after_free_2.js')])

  def test_pthread_exit_process(self):
    args = ['-pthread',
            '-sPROXY_TO_PTHREAD',
            '-sPTHREAD_POOL_SIZE=2',
            '-sEXIT_RUNTIME',
            '-DEXIT_RUNTIME',
            '-O0']
    args += ['--pre-js', test_file('core/pthread/test_pthread_exit_runtime.pre.js')]
    self.btest('core/pthread/test_pthread_exit_runtime.c', expected='onExit status: 42', cflags=args)

  @no_safari('TODO: Fails with report_result?unexpected: [object ErrorEvent]') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_pthread_trap(self):
    create_file('pre.js', '''
    if (typeof window === 'object' && window) {
      window.addEventListener('error', function(e) {
        if (e.error && e.error.message.includes('unreachable'))
          maybeReportResultToServer("expected exception caught");
        else
          maybeReportResultToServer("unexpected: " + e);
      });
    }''')
    args = ['-pthread',
            '-sPROXY_TO_PTHREAD',
            '-sEXIT_RUNTIME',
            '--profiling-funcs',
            '--pre-js=pre.js']
    self.btest('pthread/test_pthread_trap.c', expected='expected exception caught', cflags=args)

  # Tests MAIN_THREAD_EM_ASM_INT() function call signatures.
  def test_main_thread_em_asm_signatures(self):
    self.btest_exit('core/test_em_asm_signatures.cpp')

  def test_main_thread_em_asm_signatures_pthreads(self):
    self.btest_exit('core/test_em_asm_signatures.cpp', cflags=['-O3', '-pthread', '-sPROXY_TO_PTHREAD', '-sASSERTIONS'])

  def test_main_thread_async_em_asm(self):
    self.btest_exit('core/test_main_thread_async_em_asm.cpp', cflags=['-O3', '-pthread', '-sPROXY_TO_PTHREAD', '-sASSERTIONS'])

  def test_main_thread_em_asm_blocking(self):
    copy_asset('browser/test_em_asm_blocking.html', 'page.html')

    self.compile_btest('browser/test_em_asm_blocking.cpp', ['-O2', '-o', 'wasm.js', '-pthread', '-sPROXY_TO_PTHREAD', '-sEXIT_RUNTIME'])
    self.run_browser('page.html', '/report_result?exit:8')

  # Test that it is possible to send a signal via calling alarm(timeout), which in turn calls to the signal handler set by signal(SIGALRM, func);
  def test_sigalrm(self):
    self.btest_exit('test_sigalrm.c', cflags=['-O3'])

  @parameterized({
    '': ([], 1),
    'O1': (['-O1'], 1),
    'O2': (['-O2'], 1),
    'O3': (['-O3'], 1),
    # force it on
    'force': (['-sWASM_ASYNC_COMPILATION'], 1),
    'off': (['-sWASM_ASYNC_COMPILATION=0'], 0),
  })
  def test_async_compile(self, opts, returncode):
    # notice when we use async compilation
    create_file('pre.js', '''
      // note if we do async compilation
      var real_wasm_instantiate = WebAssembly.instantiate;
      var real_wasm_instantiateStreaming = WebAssembly.instantiateStreaming;
      if (typeof real_wasm_instantiateStreaming === 'function') {
        WebAssembly.instantiateStreaming = (a, b) => {
          err('instantiateStreaming called');
          console.log('instantiateStreaming called');
          Module.sawAsyncCompilation = true;
          return real_wasm_instantiateStreaming(a, b);
        };
      } else {
        WebAssembly.instantiate = (a, b) => {
          err('instantiate called');
          Module.sawAsyncCompilation = true;
          return real_wasm_instantiate(a, b);
        };
      }
    ''')
    self.cflags.append('--pre-js=pre.js')
    self.btest_exit('test_async_compile.c', assert_returncode=returncode, cflags=opts)
    # Ensure that compilation still works and is async without instantiateStreaming available
    create_file('pre0.js', 'WebAssembly.instantiateStreaming = undefined;')
    self.cflags.insert(0, '--pre-js=pre0.js')
    self.btest_exit('test_async_compile.c', assert_returncode=1)

  # Test that implementing Module.instantiateWasm() callback works.
  @also_with_asan
  def test_manual_wasm_instantiate(self):
    self.set_setting('EXIT_RUNTIME')
    self.compile_btest('test_manual_wasm_instantiate.c', ['-o', 'manual_wasm_instantiate.js'], reporting=Reporting.JS_ONLY)
    copy_asset('test_manual_wasm_instantiate.html')
    self.run_browser('test_manual_wasm_instantiate.html', '/report_result?exit:0')

  def test_wasm_locate_file(self):
    # Test that it is possible to define "Module.locateFile(foo)" function to locate where worker.js will be loaded from.
    ensure_dir('cdn')
    shell = read_file(path_from_root('html/shell.html'))
    create_file('shell2.html', shell.replace('var Module = {', 'var Module = { locateFile: (filename) => (filename == "test.wasm") ? "cdn/test.wasm" : filename, '))
    self.compile_btest('browser_test_hello_world.c', ['--shell-file', 'shell2.html', '-o', 'test.html'])
    shutil.move('test.wasm', Path('cdn/test.wasm'))
    self.run_browser('test.html', '/report_result?0')

  @also_with_threads
  def test_utf8_textdecoder(self):
    self.btest_exit('benchmark/benchmark_utf8.c', 0, cflags=['--embed-file', test_file('utf8_corpus.txt') + '@/utf8_corpus.txt'])

  @also_with_threads
  def test_utf16_textdecoder(self):
    self.btest_exit('benchmark/benchmark_utf16.cpp', 0, cflags=['--embed-file', test_file('utf16_corpus.txt') + '@/utf16_corpus.txt', '-sEXPORTED_RUNTIME_METHODS=UTF16ToString,stringToUTF16,lengthBytesUTF16'])

  # Tests that it is possible to initialize and render WebGL content in a
  # pthread by using OffscreenCanvas.
  @no_chrome('https://crbug.com/961765')
  # The non-chained version suffers from browser priority inversion deadlock problem: offscreenCanvas.getContext("webgl2") does not make progress in a pthread until main thread yields to event loop.
  # The chained version of this test suffers from bug https://bugzil.la/1992576
  @no_firefox('https://bugzil.la/1972240 (priority inversion deadlock) + https://bugzil.la/1992576 (chained OffscreenCanvas transfer)')
  @parameterized({
    '': ([],),
    # -DTEST_CHAINED_WEBGL_CONTEXT_PASSING:
    # Tests that it is possible to transfer WebGL canvas in a chain from main
    # thread -> thread 1 -> thread 2 and then init and render WebGL content there.
    'chained': (['-DTEST_CHAINED_WEBGL_CONTEXT_PASSING'],),
  })
  @requires_offscreen_canvas
  @requires_graphics_hardware
  def test_webgl_offscreen_canvas_in_pthread(self, args):
    self.btest('gl_in_pthread.c', expected='1', cflags=args + ['-pthread', '-sPTHREAD_POOL_SIZE=2', '-sOFFSCREENCANVAS_SUPPORT', '-lGL'])

  # Tests that it is possible to render WebGL content on a <canvas> on the main
  # thread, after it has once been used to render WebGL content in a pthread
  # first -DTEST_MAIN_THREAD_EXPLICIT_COMMIT: Test the same (WebGL on main
  # thread after pthread), but by using explicit .commit() to swap on the main
  # thread instead of implicit "swap when rAF ends" logic
  @parameterized({
    '': ([],),
    'explicit': (['-DTEST_MAIN_THREAD_EXPLICIT_COMMIT'],),
  })
  @requires_offscreen_canvas
  @requires_graphics_hardware
  @disabled('This test is disabled because current OffscreenCanvas does not allow transfering it after a rendering context has been created for it.')
  def test_webgl_offscreen_canvas_in_mainthread_after_pthread(self, args):
    self.btest('gl_in_mainthread_after_pthread.c', expected='0', cflags=args + ['-pthread', '-sPTHREAD_POOL_SIZE=2', '-sOFFSCREENCANVAS_SUPPORT', '-lGL'])

  @requires_offscreen_canvas
  @requires_graphics_hardware
  def test_webgl_offscreen_canvas_only_in_pthread(self):
    self.btest_exit('gl_only_in_pthread.c', cflags=['-pthread', '-sPTHREAD_POOL_SIZE', '-sOFFSCREENCANVAS_SUPPORT', '-lGL', '-sOFFSCREEN_FRAMEBUFFER'])

  # Tests that rendering from client side memory without default-enabling extensions works.
  @requires_graphics_hardware
  def test_webgl_from_client_side_memory_without_default_enabled_extensions(self):
    self.btest_exit('webgl_draw_triangle.c', cflags=['-lGL', '-sOFFSCREEN_FRAMEBUFFER', '-DEXPLICIT_SWAP=1', '-DDRAW_FROM_CLIENT_MEMORY=1', '-sFULL_ES2'])

  # Tests for WEBGL_multi_draw extension
  # For testing WebGL draft extensions like this, if using chrome as the browser,
  # We might want to append the --enable-webgl-draft-extensions to the EMTEST_BROWSER env arg.
  @requires_graphics_hardware
  @no_2gb('https://crbug.com/324562920')
  @no_4gb('https://crbug.com/324562920')
  @parameterized({
    'arrays': (['-DMULTI_DRAW_ARRAYS'],),
    'arrays_instanced': (['-DMULTI_DRAW_ARRAYS_INSTANCED'],),
    'elements': (['-DMULTI_DRAW_ELEMENTS'],),
    'elements_instanced': (['-DMULTI_DRAW_ELEMENTS_INSTANCED'],),
  })
  def test_webgl_multi_draw(self, args):
    self.reftest('webgl_multi_draw_test.c', 'webgl_multi_draw.png',
                 cflags=['-lGL', '-sOFFSCREEN_FRAMEBUFFER', '-DEXPLICIT_SWAP'] + args)

  # Tests for base_vertex/base_instance extension
  # For testing WebGL draft extensions like this, if using chrome as the browser,
  # We might want to append the --enable-webgl-draft-extensions to the EMTEST_BROWSER env arg.
  # If testing on Mac, you also need --use-cmd-decoder=passthrough to get this extension.
  # Also there is a known bug with Mac Intel baseInstance which can fail producing the expected image result.
  @requires_webgl2
  @parameterized({
    '': (0,),
    'multidraw': (1,),
  })
  @parameterized({
    '': (0,),
    'drawelements': (1,),
  })
  def test_webgl_draw_base_vertex_base_instance(self, multi_draw, draw_elements):
    self.reftest('webgl_draw_base_vertex_base_instance_test.c', 'webgl_draw_instanced_base_vertex_base_instance.png',
                 cflags=['-lGL',
                            '-sMAX_WEBGL_VERSION=2',
                            '-sOFFSCREEN_FRAMEBUFFER',
                            '-DMULTI_DRAW=' + str(multi_draw),
                            '-DDRAW_ELEMENTS=' + str(draw_elements),
                            '-DEXPLICIT_SWAP=1',
                            '-DWEBGL_CONTEXT_VERSION=2'])

  @requires_graphics_hardware
  def test_webgl_sample_query(self):
    self.btest_exit('webgl_sample_query.c', cflags=['-sMAX_WEBGL_VERSION=2', '-lGL'])

  @requires_graphics_hardware
  @parameterized({
    # EXT query entrypoints on WebGL 1.0
    '': (['-sMAX_WEBGL_VERSION'],),
    # EXT query entrypoints on a WebGL 1.0 context while built for WebGL 2.0
    'v2': (['-sMAX_WEBGL_VERSION=2'],),
    # builtin query entrypoints on WebGL 2.0
    'v2api': (['-sMAX_WEBGL_VERSION=2', '-DTEST_WEBGL2'],),
  })
  def test_webgl_timer_query(self, args):
    self.btest_exit('webgl_timer_query.c', cflags=args + ['-lGL'])

  # Tests that -sOFFSCREEN_FRAMEBUFFER rendering works.
  @requires_graphics_hardware
  @parameterized({
    '': ([],),
    'threads': (['-pthread', '-sPROXY_TO_PTHREAD'],),
  })
  @parameterized({
    '': ([],),
    'es2': (['-sFULL_ES2'],),
    'es3': (['-sFULL_ES3'],),
  })
  def test_webgl_offscreen_framebuffer(self, version, threads):
    # Tests all the different possible versions of libgl
    args = ['-lGL', '-sOFFSCREEN_FRAMEBUFFER', '-DEXPLICIT_SWAP=1'] + threads + version
    self.btest_exit('webgl_draw_triangle.c', cflags=args)

  # Tests that VAOs can be used even if WebGL enableExtensionsByDefault is set to 0.
  @requires_graphics_hardware
  def test_webgl_vao_without_automatic_extensions(self):
    self.btest_exit('test_webgl_no_auto_init_extensions.c', cflags=['-lGL', '-sGL_SUPPORT_AUTOMATIC_ENABLE_EXTENSIONS=0'])

  # Tests that offscreen framebuffer state restoration works
  @requires_graphics_hardware
  @parameterized({
    # full state restoration path on WebGL 1.0
    'gl1_no_vao': (['-sMAX_WEBGL_VERSION=1', '-DTEST_DISABLE_VAO'],),
    # VAO path on WebGL 1.0
    'gl1': (['-sMAX_WEBGL_VERSION=1', '-DTEST_VERIFY_WEBGL1_VAO_SUPPORT=1'],),
    'gl1_max_gl2': (['-sMAX_WEBGL_VERSION=2'],),
    # VAO path on WebGL 2.0
    'gl2': (['-sMAX_WEBGL_VERSION=2', '-DTEST_WEBGL2=1', '-DTEST_ANTIALIAS=1'],),
    # full state restoration path on WebGL 2.0
    'gl2_no_vao': (['-sMAX_WEBGL_VERSION=2', '-DTEST_WEBGL2=1', '-DTEST_ANTIALIAS=1', '-DTEST_DISABLE_VAO'],),
    # blitFramebuffer path on WebGL 2.0 (falls back to VAO on Firefox < 67)
    'gl2_no_aa': (['-sMAX_WEBGL_VERSION=2', '-DTEST_WEBGL2=1', '-DTEST_ANTIALIAS=0'],),
  })
  def test_webgl_offscreen_framebuffer_state_restoration(self, args):
    if '-DTEST_WEBGL2=1' in args and webgl2_disabled():
      self.skipTest('This test requires WebGL2 to be available')
    base_args = ['-lGL', '-sOFFSCREEN_FRAMEBUFFER', '-DEXPLICIT_SWAP=1']
    self.btest_exit('webgl_offscreen_framebuffer_swap_with_bad_state.c', cflags=base_args + args)

  @parameterized({
    '': ([],),
    'es2': (['-sFULL_ES2'],),
    'es3': (['-sFULL_ES3'],),
  })
  @requires_graphics_hardware
  def test_webgl_draw_triangle_with_uniform_color(self, args):
    self.btest_exit('webgl_draw_triangle_with_uniform_color.c', cflags=args)

  # Tests that using an array of structs in GL uniforms works.
  @requires_webgl2
  def test_webgl_array_of_structs_uniform(self):
    self.reftest('webgl_array_of_structs_uniform.c', cflags=['-lGL', '-sMAX_WEBGL_VERSION=2'])

  # Tests that if a WebGL context is created in a pthread on a canvas that has
  # not been transferred to that pthread, WebGL calls are then proxied to the
  # main thread -DTEST_OFFSCREEN_CANVAS=1: Tests that if a WebGL context is
  # created on a pthread that has the canvas transferred to it via using
  # Emscripten's EMSCRIPTEN_PTHREAD_TRANSFERRED_CANVASES="#canvas", then
  # OffscreenCanvas is used -DTEST_OFFSCREEN_CANVAS=2: Tests that if a WebGL
  # context is created on a pthread that has the canvas transferred to it via
  # automatic transferring of Module.canvas when
  # EMSCRIPTEN_PTHREAD_TRANSFERRED_CANVASES is not defined, then OffscreenCanvas
  # is also used
  @parameterized({
    '': ([False],),
    'asyncify': ([True],),
  })
  @requires_offscreen_canvas
  @requires_graphics_hardware
  def test_webgl_offscreen_canvas_in_proxied_pthread(self, asyncify):
    args = ['-pthread', '-sOFFSCREENCANVAS_SUPPORT', '-lGL', '-sGL_DEBUG', '-sPROXY_TO_PTHREAD']
    if asyncify:
      # given the synchronous render loop here, asyncify is needed to see intermediate frames and
      # the gradual color change
      args += ['-sASYNCIFY', '-DASYNCIFY']
    self.btest_exit('gl_in_proxy_pthread.c', cflags=args)

  @parameterized({
    '': ([],),
    'proxy': (['-sPROXY_TO_PTHREAD'],),
  })
  @parameterized({
    '': ([],),
    'blocking': (['-DTEST_SYNC_BLOCKING_LOOP=1'],),
  })
  @parameterized({
    '': ([],),
    'offscreen': (['-sOFFSCREENCANVAS_SUPPORT', '-sOFFSCREEN_FRAMEBUFFER'],),
  })
  @requires_graphics_hardware
  @requires_offscreen_canvas
  def test_webgl_resize_offscreencanvas_from_main_thread(self, args1, args2, args3):
    cmd = args1 + args2 + args3 + ['-pthread', '-lGL', '-sGL_DEBUG']

    if is_firefox() and '-sOFFSCREENCANVAS_SUPPORT' in cmd and '-sPROXY_TO_PTHREAD' in cmd:
      # Firefox is unable to transfer the same OffscreenCanvas multiple times across Workers
      # (in a chained fashion, e.g. main thread -> proxy-to-pthread main thread -> back to main thread -> user pthread)
      self.skipTest('https://bugzil.la/1992576')

    print(str(cmd))
    self.btest_exit('test_webgl_resize_offscreencanvas_from_main_thread.c', cflags=cmd)

  @requires_graphics_hardware
  @parameterized({
    'v1': (1,),
    'v2': (2,),
  })
  @parameterized({
    'enable': (1,),
    'disable': (0,),
  })
  def test_webgl_simple_extensions(self, webgl_version, simple_enable_extensions):
    if webgl_version == 2 and webgl2_disabled():
      self.skipTest('This test requires WebGL2 to be available')

    cmd = ['-DWEBGL_CONTEXT_VERSION=' + str(webgl_version),
           '-DWEBGL_SIMPLE_ENABLE_EXTENSION=' + str(simple_enable_extensions),
           '-sMAX_WEBGL_VERSION=2',
           '-sGL_SUPPORT_AUTOMATIC_ENABLE_EXTENSIONS=' + str(simple_enable_extensions),
           '-sGL_SUPPORT_SIMPLE_ENABLE_EXTENSIONS=' + str(simple_enable_extensions)]
    self.btest_exit('webgl2_simple_enable_extensions.c', cflags=cmd)

  @parameterized({
    '': ([],),
    'closure': (['-sASSERTIONS', '--closure=1'],),
    'closure_advanced': (['-sASSERTIONS', '--closure=1', '-O3'],),
    'main_module': (['-sMAIN_MODULE=1'],),
    'pthreads': (['-pthread', '-sOFFSCREENCANVAS_SUPPORT'],),
  })
  @requires_webgpu
  def test_webgpu_basic_rendering(self, args):
    self.btest_exit('webgpu_basic_rendering.cpp', cflags=['--use-port=emdawnwebgpu', '-sEXIT_RUNTIME'] + args)

  @requires_webgpu
  def test_webgpu_required_limits(self):
    self.set_setting('DEFAULT_TO_CXX')  # emdawnwebgpu uses C++ internally
    self.btest_exit('webgpu_required_limits.c', cflags=['--use-port=emdawnwebgpu'])

  # Tests the feature that shell html page can preallocate the typed array and place it
  # to Module.buffer before loading the script page.
  # In this build mode, the -sINITIAL_MEMORY=xxx option will be ignored.
  # Preallocating the buffer in this was is asm.js only (wasm needs a Memory).
  @requires_wasm2js
  def test_preallocated_heap(self):
    self.btest_exit('test_preallocated_heap.cpp', cflags=['-sWASM=0', '-sIMPORTED_MEMORY', '-sINITIAL_MEMORY=16MB', '-sABORTING_MALLOC=0', '--shell-file', test_file('browser/test_preallocated_heap_shell.html')])

  # Tests emscripten_fetch() usage to XHR data directly to memory without persisting results to IndexedDB.
  @also_with_wasm2js
  @also_with_fetch_streaming
  def test_fetch_to_memory(self):
    # Test error reporting in the negative case when the file URL doesn't exist. (http 404)
    self.btest_exit('fetch/test_fetch_to_memory.cpp',
                    cflags=['-sFETCH_DEBUG', '-sFETCH', '-DFILE_DOES_NOT_EXIST'])

    # Test the positive case when the file URL exists. (http 200)
    copy_asset('gears.png')
    for arg in ([], ['-sFETCH_SUPPORT_INDEXEDDB=0']):
      self.btest_exit('fetch/test_fetch_to_memory.cpp',
                      cflags=['-sFETCH_DEBUG', '-sFETCH'] + arg)

  @also_with_wasm2js
  @also_with_fetch_streaming
  @parameterized({
    '': ([],),
    'pthread_exit': (['-DDO_PTHREAD_EXIT'],),
  })
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/16868')
  def test_fetch_from_thread(self, args):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_from_thread.cpp',
                    cflags=args + ['-pthread', '-sPROXY_TO_PTHREAD', '-sFETCH_DEBUG', '-sFETCH', '-DFILE_DOES_NOT_EXIST'])

  @also_with_wasm2js
  @also_with_fetch_streaming
  def test_fetch_to_indexdb(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_to_indexeddb.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH'])

  # Tests emscripten_fetch() usage to persist an XHR into IndexedDB and subsequently load up from there.
  @also_with_wasm2js
  @also_with_fetch_streaming
  def test_fetch_cached_xhr(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_cached_xhr.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH'])

  # Tests that response headers get set on emscripten_fetch_t values.
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/16868')
  @also_with_wasm2js
  @also_with_fetch_streaming
  @parameterized({
    '': ([],),
    'sync': (['-DSYNC'],),
  })
  def test_fetch_response_headers(self, args):
    if self.get_setting('FETCH_STREAMING') and '-DSYNC' in args:
      self.skipTest('Fetch backend does not support sync fetch.')
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_response_headers.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH', '-pthread', '-sPROXY_TO_PTHREAD'] + args)

  def make_largefile(self):
    s = '12345678'
    for _ in range(14):
      s = s[::-1] + s # length of str will be 2^17=128KB
    with open('largefile.txt', 'w', encoding='utf-8') as f:
      for _ in range(1024):
        f.write(s)

  # Test emscripten_fetch() usage to stream a fetch in to memory without storing the full file in memory
  # Streaming only works the fetch backend.
  @also_with_wasm2js
  def test_fetch_stream_file(self):
    # Strategy: create a large 128MB file, and compile with a small 16MB Emscripten heap, so that the tested file
    # won't fully fit in the heap. This verifies that streaming works properly.
    self.make_largefile()
    self.btest_exit('fetch/test_fetch_stream_file.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH', '-sFETCH_STREAMING'])

  @also_with_fetch_streaming
  def test_fetch_headers_received(self):
    create_file('myfile.dat', 'hello world\n')
    self.btest_exit('fetch/test_fetch_headers_received.c', cflags=['-sFETCH_DEBUG', '-sFETCH'])

  @also_with_fetch_streaming
  def test_fetch_xhr_abort(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_xhr_abort.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH'])

  # Tests emscripten_fetch() usage in synchronous mode when used from the main
  # thread proxied to a Worker with -sPROXY_TO_PTHREAD option.
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/16868')
  @also_with_wasm2js
  def test_fetch_sync_xhr(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_sync_xhr.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH', '-pthread', '-sPROXY_TO_PTHREAD'])

  # Tests synchronous emscripten_fetch() usage from pthread
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/16868')
  def test_fetch_sync(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_sync.c', cflags=['-sFETCH', '-pthread', '-sPROXY_TO_PTHREAD'])

  # Tests that the Fetch API works for synchronous XHRs when program is run in a worker
  @no_firefox('https://github.com/emscripten-core/emscripten/issues/16868')
  @also_with_wasm2js
  def test_fetch_sync_xhr_in_proxy_to_worker(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_sync_xhr.cpp', cflags=['-sFETCH_DEBUG', '-sFETCH'], run_in_worker=True)

  @disabled('https://github.com/emscripten-core/emscripten/issues/16746')
  def test_fetch_idb_store(self):
    self.btest_exit('fetch/test_fetch_idb_store.cpp', cflags=['-pthread', '-sFETCH', '-sPROXY_TO_PTHREAD'])

  @disabled('https://github.com/emscripten-core/emscripten/issues/16746')
  def test_fetch_idb_delete(self):
    copy_asset('gears.png')
    self.btest_exit('fetch/test_fetch_idb_delete.cpp', cflags=['-pthread', '-sFETCH_DEBUG', '-sFETCH', '-sWASM=0', '-sPROXY_TO_PTHREAD'])

  @also_with_fetch_streaming
  def test_fetch_post(self):
    self.btest_exit('fetch/test_fetch_post.c', cflags=['-sFETCH'])

  @also_with_fetch_streaming
  def test_fetch_progress(self):
    create_file('myfile.dat', 'hello world\n' * 1000)
    self.btest_exit('fetch/test_fetch_progress.c', cflags=['-sFETCH'])

  @also_with_fetch_streaming
  def test_fetch_to_memory_async(self):
    create_file('myfile.dat', 'hello world\n' * 1000)
    self.btest_exit('fetch/test_fetch_to_memory_async.c', cflags=['-sFETCH'])

  @requires_shared_array_buffer
  def test_fetch_to_memory_sync(self):
    create_file('myfile.dat', 'hello world\n' * 1000)
    self.btest_exit('fetch/test_fetch_to_memory_sync.c', cflags=['-sFETCH', '-pthread', '-sPROXY_TO_PTHREAD'])

  # Streaming only works the fetch backend.
  def test_fetch_stream_async(self):
    create_file('myfile.dat', 'hello world\n' * 1000)
    self.btest_exit('fetch/test_fetch_stream_async.c', cflags=['-sFETCH', '-sFETCH_STREAMING'])

  @also_with_asan
  def test_fetch_stream_abort(self):
    self.make_largefile()
    self.btest_exit('fetch/test_fetch_stream_abort.cpp', cflags=['-sFETCH', '-sFETCH_STREAMING', '-sALLOW_MEMORY_GROWTH'])

  @also_with_fetch_streaming
  def test_fetch_persist(self):
    create_file('myfile.dat', 'hello world\n')
    self.btest_exit('fetch/test_fetch_persist.c', cflags=['-sFETCH'])

  @no_firefox('https://github.com/emscripten-core/emscripten/issues/16868')
  @also_with_fetch_streaming
  def test_fetch_redirect(self):
    self.btest_exit('fetch/test_fetch_redirect.c', cflags=['-sFETCH', '-pthread', '-sPROXY_TO_PTHREAD', f'-DSERVER="{self.SERVER_URL}"'])

  @parameterized({
    '': ([],),
    'mt': (['-pthread', '-sPTHREAD_POOL_SIZE=2'],),
  })
  def test_pthread_locale(self, args):
    self.btest_exit('pthread/test_pthread_locale.c', cflags=args)

  # Tests the Emscripten HTML5 API emscripten_set_canvas_element_size() and
  # emscripten_get_canvas_element_size() functionality in singlethreaded programs.
  def test_emscripten_set_canvas_element_size(self):
    self.btest_exit('emscripten_set_canvas_element_size.c')

  # Test that emscripten_get_device_pixel_ratio() is callable from pthreads (and proxies to main
  # thread to obtain the proper window.devicePixelRatio value).
  @also_with_proxy_to_pthread
  def test_emscripten_get_device_pixel_ratio(self):
    self.btest_exit('emscripten_get_device_pixel_ratio.c')

  # Tests that emscripten_run_script() variants of functions work in pthreads.
  @also_with_proxy_to_pthread
  def test_pthread_run_script(self):
    copy_asset('pthread/foo.js')
    self.btest_exit('pthread/test_pthread_run_script.c', cflags=['-O3'])

  # Tests emscripten_set_canvas_element_size() and OffscreenCanvas functionality in different build configurations.
  @requires_graphics_hardware
  @requires_offscreen_canvas
  @parameterized({
    '': ([], True),
    'offscreen': (['-sOFFSCREENCANVAS_SUPPORT'], True),
    'pthread': (['-sPROXY_TO_PTHREAD', '-pthread', '-sOFFSCREEN_FRAMEBUFFER'], True),
    'pthread_ofb_main_loop': (['-sPROXY_TO_PTHREAD', '-pthread', '-sOFFSCREEN_FRAMEBUFFER', '-DTEST_EXPLICIT_CONTEXT_SWAP=1'], True),
    'pthread_ofb': (['-sPROXY_TO_PTHREAD', '-pthread', '-sOFFSCREEN_FRAMEBUFFER', '-DTEST_EXPLICIT_CONTEXT_SWAP=1'], False),
    'manual_css': (['-sPROXY_TO_PTHREAD', '-pthread', '-sOFFSCREEN_FRAMEBUFFER', '-DTEST_EXPLICIT_CONTEXT_SWAP=1', '-DTEST_MANUALLY_SET_ELEMENT_CSS_SIZE=1'], False),
  })
  def test_emscripten_animate_canvas_element_size(self, args, main_loop):
    cmd = ['-lGL', '-O3', '-g2', '--shell-file', test_file('canvas_animate_resize_shell.html'), '-sGL_DEBUG', '-sASSERTIONS'] + args
    if not self.is_2gb() and not self.is_4gb():
      # Thread profiler does not yet work with large pointers.
      # https://github.com/emscripten-core/emscripten/issues/21229
      cmd.append('--threadprofiler')
    if main_loop:
      cmd.append('-DTEST_EMSCRIPTEN_SET_MAIN_LOOP=1')
    self.btest_exit('canvas_animate_resize.c', cflags=cmd)

  # Tests the absolute minimum pthread-enabled application.
  @parameterized({
    '': ([],),
    'modularize': (['-sMODULARIZE'],),
  })
  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
  })
  def test_pthread_hello_thread(self, opts, modularize):
    self.btest_exit('pthread/hello_thread.c', cflags=['-pthread'] + modularize + opts)

  # Tests that a pthreads build of -sMINIMAL_RUNTIME works well in different build modes
  @parameterized({
    '': ([],),
    'modularize': (['-sMODULARIZE', '-sEXPORT_NAME=MyModule'],),
    'O3': (['-O3'],),
    'O3_modularize': (['-O3', '-sMODULARIZE', '-sEXPORT_NAME=MyModule'],),
    'O3_modularize_MINIMAL_RUNTIME_2': (['-O3', '-sMODULARIZE', '-sEXPORT_NAME=MyModule', '-sMINIMAL_RUNTIME=2'],),
  })
  def test_minimal_runtime_hello_thread(self, opts):
    self.btest_exit('pthread/hello_thread.c', cflags=['--closure=1', '-sMINIMAL_RUNTIME', '-pthread'] + opts)

  # Tests memory growth in pthreads mode, but still on the main thread.
  @parameterized({
    '': ([], 1),
    'growable_arraybuffers': (['-sGROWABLE_ARRAYBUFFERS', '-Wno-experimental'], 1),
    'proxy': (['-sPROXY_TO_PTHREAD', '-sEXIT_RUNTIME'], 2),
  })
  @no_2gb('uses INITIAL_MEMORY')
  @no_4gb('uses INITIAL_MEMORY')
  @requires_growable_arraybuffers
  def test_pthread_growth_mainthread(self, cflags, pthread_pool_size):
    self.set_setting('PTHREAD_POOL_SIZE', pthread_pool_size)
    if '-sGROWABLE_ARRAYBUFFERS' not in cflags:
      self.cflags.append('-Wno-pthreads-mem-growth')
    self.btest_exit('pthread/test_pthread_memory_growth_mainthread.c', cflags=['-pthread', '-sALLOW_MEMORY_GROWTH', '-sINITIAL_MEMORY=32MB', '-sMAXIMUM_MEMORY=256MB'] + cflags)

  # Tests memory growth in a pthread.
  @parameterized({
    '': ([],),
    'growable_arraybuffers': (['-sGROWABLE_ARRAYBUFFERS', '-Wno-experimental'],),
    'assert': (['-sASSERTIONS'],),
    'proxy': (['-sPROXY_TO_PTHREAD'], 2),
    'minimal': (['-sMINIMAL_RUNTIME', '-sMODULARIZE', '-sEXPORT_NAME=MyModule'],),
  })
  @no_2gb('uses INITIAL_MEMORY')
  @no_4gb('uses INITIAL_MEMORY')
  @requires_growable_arraybuffers
  def test_pthread_growth(self, cflags, pthread_pool_size=1):
    self.set_setting('PTHREAD_POOL_SIZE', pthread_pool_size)
    if '-sGROWABLE_ARRAYBUFFERS' not in cflags:
      self.cflags.append('-Wno-pthreads-mem-growth')
    self.btest_exit('pthread/test_pthread_memory_growth.c', cflags=['-pthread', '-sALLOW_MEMORY_GROWTH', '-sINITIAL_MEMORY=32MB', '-sMAXIMUM_MEMORY=256MB'] + cflags)

  # Tests that time in a pthread is relative to the main thread, so measurements
  # on different threads are still monotonic, as if checking a single central
  # clock.
  def test_pthread_reltime(self):
    self.btest_exit('pthread/test_pthread_reltime.cpp', cflags=['-pthread', '-sPTHREAD_POOL_SIZE'])

  # Tests that it is possible to load the main .js file of the application manually via mainScriptUrlOrBlob,
  # and still use pthreads.
  @parameterized({
    'blob': (False, True),
    'url': (False, False),
    'blob_es6': (True, True),
    'url_es6': (True, False),
  })
  @requires_es6_workers
  def test_mainScriptUrlOrBlob(self, es6, use_blob):
    # TODO: enable this with wasm, currently pthreads/atomics have limitations
    self.set_setting('EXIT_RUNTIME')
    js_name = 'hello_thread_with_loader.%s' % ('mjs' if es6 else 'js')
    if es6:
      self.cflags += ['-sEXPORT_ES6']
    if es6 and use_blob:
      create_file('loader.mjs', '''
        Module['locateFile'] = (path,_prefix) => path;
        let blob = await (await fetch('hello_thread_with_loader.mjs')).blob();
        Module['mainScriptUrlOrBlob'] = blob;
        (await import(URL.createObjectURL(blob))).default(Module);
      ''')
    elif use_blob:
      create_file('loader.mjs', '''
        let blob = await (await fetch('hello_thread_with_loader.js')).blob();
        Module['mainScriptUrlOrBlob'] = blob;
        var script = document.createElement('script');
        script.src = URL.createObjectURL(blob);
        document.body.appendChild(script);
      ''')
    elif es6:
      create_file('loader.mjs', '''
        Module['mainScriptUrlOrBlob'] = 'hello_thread_with_loader.mjs';
        (await import('./hello_thread_with_loader.mjs')).default(Module);
      ''')
    else:
      create_file('loader.mjs', '''
        var script = document.createElement('script');
        Module['mainScriptUrlOrBlob'] = 'hello_thread_with_loader.js';
        script.src = Module['mainScriptUrlOrBlob'];
        document.body.appendChild(script);
      ''')

    self.compile_btest('pthread/hello_thread.c', ['-pthread', '-o', 'out.js'], reporting=Reporting.JS_ONLY)

    # Now run the test with the JS file renamed and with its content
    # stored in Module['mainScriptUrlOrBlob'].
    shutil.move('out.js', js_name)
    copy_asset('pthread/main_js_with_loader.html', 'hello_thread_with_loader.html')
    self.run_browser('hello_thread_with_loader.html', '/report_result?exit:0')

  # Tests that SINGLE_FILE works as intended in generated HTML (with and without Worker)
  def test_single_file_html(self):
    self.btest('single_file_static_initializer.cpp', '19', cflags=['-sSINGLE_FILE'])
    self.assertExists('test.html')
    self.assertNotExists('test.js')
    self.assertNotExists('test.worker.js')
    self.assertNotExists('test.wasm')

  # Tests that SINGLE_FILE works as intended in generated HTML with MINIMAL_RUNTIME
  @also_with_wasm2js
  @parameterized({
    '': ([],),
    'O3': (['-O3'],),
  })
  def test_minimal_runtime_single_file_html(self, opts):
    self.btest('single_file_static_initializer.cpp', '19', cflags=opts + ['-sMINIMAL_RUNTIME', '-sSINGLE_FILE'])
    self.assertExists('test.html')
    self.assertNotExists('test.js')
    self.assertNotExists('test.wasm')
    self.assertNotExists('test.asm.js')
    self.assertNotExists('test.js')
    self.assertNotExists('test.worker.js')

  # Tests that SINGLE_FILE works when built with ENVIRONMENT=web and Closure enabled (#7933)
  def test_single_file_in_web_environment_with_closure(self):
    self.btest_exit('minimal_hello.c', cflags=['-sSINGLE_FILE', '-sENVIRONMENT=web', '-O2', '--closure=1'])

  # Tests that SINGLE_FILE works as intended with locateFile
  @also_with_wasm2js
  def test_single_file_locate_file(self):
    self.compile_btest('browser_test_hello_world.c', ['-o', 'test.js', '-sSINGLE_FILE'])

    create_file('test.html', '''
      <script>
        var Module = {
          locateFile: function (path) {
            if (path.startsWith('data:')) {
              throw new Error('Unexpected data URI.');
            }

            return path;
          }
        };
      </script>
      <script src="test.js"></script>
    ''')

    self.run_browser('test.html', '/report_result?0')

  # Tests that SINGLE_FILE works as intended in a Worker in JS output
  def test_single_file_worker_js(self):
    self.btest_exit('browser_test_hello_world.c', cflags=['-sSINGLE_FILE'], run_in_worker=True)

  # Tests that pthreads code works as intended in a Worker. That is, a pthreads-using
  # program can run either on the main thread (normal tests) or when we start it in
  # a Worker in this test (in that case, both the main application thread and the worker
  # threads are all inside Web Workers).
  @parameterized({
    '': ([],),
    'limited_env': (['-sENVIRONMENT=worker'],),
  })
  def test_pthreads_started_in_worker(self, args):
    self.btest_exit('pthread/test_pthread_atomics.c', cflags=['-o', 'test.js', '-pthread', '-sPTHREAD_POOL_SIZE=8'] + args, run_in_worker=True)

  def test_access_file_after_heap_resize(self):
    create_file('test.txt', 'hello from file')
    self.btest_exit('access_file_after_heap_resize.c', cflags=['-sALLOW_MEMORY_GROWTH', '--preload-file', 'test.txt'])

    # with separate file packager invocation
    self.run_process([FILE_PACKAGER, 'data.data', '--preload', 'test.txt', '--js-output=' + 'data.js'])
    self.btest_exit('access_file_after_heap_resize.c', cflags=['-sALLOW_MEMORY_GROWTH', '--pre-js', 'data.js', '-sFORCE_FILESYSTEM'])

  def test_unicode_html_shell(self):
    create_file('main.c', r'''
      int main() {
        return 0;
      }
    ''')
    create_file('shell.html', read_file(path_from_root('html/shell.html')).replace('Emscripten-Generated Code', 'Emscripten-Generated Emoji 😅'))
    self.btest_exit('main.c', cflags=['--shell-file', 'shell.html'])

  # Tests the functionality of the emscripten_thread_sleep() function.
  def test_emscripten_thread_sleep(self):
    self.btest_exit('pthread/test_emscripten_thread_sleep.c', cflags=['-pthread'])

  # Tests that Emscripten-compiled applications can be run from a relative path in browser that is different than the address of the current page
  def test_browser_run_from_different_directory(self):
    self.compile_btest('browser_test_hello_world.c', ['-o', 'test.html', '-O3'])

    ensure_dir('subdir')
    shutil.move('test.js', Path('subdir/test.js'))
    shutil.move('test.wasm', Path('subdir/test.wasm'))
    src = read_file('test.html')
    # Make sure JS is loaded from subdirectory
    create_file('test-subdir.html', src.replace('test.js', 'subdir/test.js'))
    self.run_browser('test-subdir.html', '/report_result?0')

  # Similar to `test_browser_run_from_different_directory`, but asynchronous because of `-sMODULARIZE`
  def test_browser_run_from_different_directory_async(self):
    # compile the code with the modularize feature and the preload-file option enabled
    self.compile_btest('browser_test_hello_world.c', ['-o', 'test.js', '-O3', '-sMODULARIZE'])
    ensure_dir('subdir')
    shutil.move('test.js', Path('subdir/test.js'))
    shutil.move('test.wasm', Path('subdir/test.wasm'))
    # Make sure JS is loaded from subdirectory
    create_file('test-subdir.html', '''
      <script src="subdir/test.js"></script>
      <script>
        Module();
      </script>
    ''')
    self.run_browser('test-subdir.html', '/report_result?0')

  # Similar to `test_browser_run_from_different_directory`, but
  # also also we eval the initial code, so currentScript is not present. That prevents us
  # from finding the file in a subdir, but here we at least check we do not regress compared to the
  # normal case of finding in the current dir.
  # test both modularize (and creating an instance) and modularize-instance
  # (which creates by itself)
  @parameterized({
    '': ([], ['-sMODULARIZE'], 'Module();'),
    'subdir': (['subdir'], ['-sMODULARIZE'], 'Module();'),
  })
  def test_browser_modularize_no_current_script(self, path, args, creation):
    filesystem_path = os.path.join('.', *path)
    ensure_dir(filesystem_path)
    # compile the code with the modularize feature and the preload-file option enabled
    self.compile_btest('browser_test_hello_world.c', ['-o', 'test.js'] + args)
    shutil.move('test.js', Path(filesystem_path, 'test.js'))
    shutil.move('test.wasm', Path(filesystem_path, 'test.wasm'))
    create_file(Path(filesystem_path, 'test.html'), '''
      <script>
        setTimeout(async () => {
          let response = await fetch('test.js');
          let text = await response.text();
          eval(text);
          %s
        }, 1);
      </script>
    ''' % creation)
    self.run_browser('/'.join(path + ['test.html']), '/report_result?0')

  def test_emscripten_request_animation_frame(self):
    self.btest_exit('emscripten_request_animation_frame.c')

  def test_emscripten_request_animation_frame_loop(self):
    self.btest_exit('emscripten_request_animation_frame_loop.c')

  def test_request_animation_frame(self):
    self.btest_exit('test_request_animation_frame.c')

  @requires_shared_array_buffer
  def test_emscripten_set_timeout(self):
    self.btest_exit('emscripten_set_timeout.c', cflags=['-pthread', '-sPROXY_TO_PTHREAD'])

  @requires_shared_array_buffer
  def test_emscripten_set_timeout_loop(self):
    self.btest_exit('emscripten_set_timeout_loop.c', cflags=['-pthread', '-sPROXY_TO_PTHREAD'])

  def test_emscripten_set_immediate(self):
    self.btest_exit('emscripten_set_immediate.c')

  def test_emscripten_set_immediate_loop(self):
    self.btest_exit('emscripten_set_immediate_loop.c')

  @requires_shared_array_buffer
  def test_emscripten_set_interval(self):
    self.btest_exit('emscripten_set_interval.c', cflags=['-pthread', '-sPROXY_TO_PTHREAD'])

  @parameterized({
    '': ([],),
    'pthread': (['-pthread', '-sPROXY_TO_PTHREAD'],),
  })
  def test_emscripten_performance_now(self, args):
    # Test emscripten_performance_now() and emscripten_date_now()
    self.btest_exit('emscripten_performance_now.c', cflags=args)

  def test_embind_with_pthreads(self):
    self.btest_exit('embind/test_pthreads.cpp', cflags=['-lembind', '-pthread', '-sPTHREAD_POOL_SIZE=2'])

  @parameterized({
    'asyncify': (['-sASYNCIFY'],),
    'jspi': (['-sJSPI', '-Wno-experimental'],),
  })
  def test_embind(self, args):
    if is_jspi(args) and not is_chrome():
      self.skipTest(f'Current browser ({get_browser()}) does not support JSPI. Only chromium-based browsers ({CHROMIUM_BASED_BROWSERS}) support JSPI today.')
    if is_jspi(args) and self.is_wasm64():
      self.skipTest('_emval_await fails')

    self.btest_exit('embind_with_asyncify.cpp', cflags=['-lembind'] + args)

  # Test emscripten_console_log(), emscripten_console_warn() and emscripten_console_error()
  def test_emscripten_console_log(self):
    self.btest_exit('emscripten_console_log.c', cflags=['--pre-js', test_file('emscripten_console_log_pre.js')])

  def test_emscripten_throw_number(self):
    self.btest('emscripten_throw_number.c', '0', cflags=['--pre-js', test_file('emscripten_throw_number_pre.js')])

  def test_emscripten_throw_string(self):
    self.btest('emscripten_throw_string.c', '0', cflags=['--pre-js', test_file('emscripten_throw_string_pre.js')])

  # Tests that Closure run in combination with -sENVIRONMENT=web mode works with a minimal console.log() application
  def test_closure_in_web_only_target_environment_console_log(self):
    self.btest_exit('minimal_hello.c', cflags=['-sENVIRONMENT=web', '-O3', '--closure=1'])

  # Tests that Closure run in combination with -sENVIRONMENT=web mode works with a small WebGL application
  @requires_graphics_hardware
  def test_closure_in_web_only_target_environment_webgl(self):
    self.btest_exit('webgl_draw_triangle.c', cflags=['-lGL', '-sENVIRONMENT=web', '-O3', '--closure=1'])

  @requires_wasm2js
  @parameterized({
    '': ([],),
    'minimal': (['-sMINIMAL_RUNTIME'],),
  })
  def test_no_declare_asm_module_exports_wasm2js(self, args):
    # TODO(sbc): Fix closure warnings with MODULARIZE + WASM=0
    self.ldflags.append('-Wno-error=closure')
    self.btest_exit('declare_asm_module_exports.c', cflags=['-sDECLARE_ASM_MODULE_EXPORTS=0', '-sENVIRONMENT=web', '-O3', '--closure=1', '-sWASM=0'] + args)

  @parameterized({
    '': ([],),
    'strict_js': (['-sSTRICT_JS'],),
    'minimal_runtime': (['-sMINIMAL_RUNTIME=1'],),
    'minimal_runtime_2': (['-sMINIMAL_RUNTIME=2'],),
  })
  def test_no_declare_asm_module_exports(self, args):
    self.btest_exit('declare_asm_module_exports.c', cflags=['-sDECLARE_ASM_MODULE_EXPORTS=0', '-sENVIRONMENT=web', '-O3', '--closure=1'] + args)

  # Tests that the different code paths in html/shell_minimal_runtime.html all work ok.
  @parameterized({
    '': ([],),
    'modularize': (['-sMODULARIZE'],),
  })
  @also_with_wasm2js
  def test_minimal_runtime_loader_shell(self, args):
    args = ['-sMINIMAL_RUNTIME=2']
    self.btest_exit('minimal_hello.c', cflags=args)

  # Tests that -sMINIMAL_RUNTIME works well in different build modes
  @parameterized({
    '': ([],),
    'streaming_compile': (['-sMINIMAL_RUNTIME_STREAMING_WASM_COMPILATION', '-sENVIRONMENT=web', '--closure=1'],),
    'streaming_inst': (['-sMINIMAL_RUNTIME_STREAMING_WASM_INSTANTIATION', '-sENVIRONMENT=web', '--closure=1'],),
  })
  def test_minimal_runtime_hello_world(self, args):
    self.btest_exit('hello_world_small.c', cflags=args + ['-sMINIMAL_RUNTIME'])

  # Tests emscripten_unwind_to_js_event_loop() behavior
  def test_emscripten_unwind_to_js_event_loop(self):
    self.btest_exit('test_emscripten_unwind_to_js_event_loop.c')

  @requires_wasm2js
  @parameterized({
    '': ([],),
    'minimal': (['-sMINIMAL_RUNTIME'],),
  })
  def test_wasm2js_fallback(self, args):
    self.set_setting('EXIT_RUNTIME')
    self.compile_btest('hello_world_small.c', ['-sWASM=2', '-o', 'test.html'] + args)

    # First run with WebAssembly support enabled
    # Move the Wasm2js fallback away to test it is not accidentally getting loaded.
    os.rename('test.wasm.js', 'test.wasm.js.unused')
    self.run_browser('test.html', '/report_result?exit:0')
    os.rename('test.wasm.js.unused', 'test.wasm.js')

    # Then disable WebAssembly support in VM, and try again.. Should still work with Wasm2JS fallback.
    html = read_file('test.html')
    html = html.replace('<body>', '<body><script>delete WebAssembly;</script>')
    create_file('test.html', html)
    os.remove('test.wasm') # Also delete the Wasm file to test that it is not attempted to be loaded.
    self.run_browser('test.html', '/report_result?exit:0')

  @requires_wasm2js
  @parameterized({
    '': ([],),
    'minimal': (['-sMINIMAL_RUNTIME'],),
  })
  def test_wasm2js_fallback_on_wasm_compilation_failure(self, args):
    self.set_setting('EXIT_RUNTIME')
    self.compile_btest('hello_world_small.c', ['-sWASM=2', '-o', 'test.html'] + args)

    # Run without the .wasm.js file present: with Wasm support, the page should still run
    os.rename('test.wasm.js', 'test.wasm.js.unused')
    self.run_browser('test.html', '/report_result?exit:0')

    # Restore the .wasm.js file, then corrupt the .wasm file, that should trigger the Wasm2js fallback to run
    os.rename('test.wasm.js.unused', 'test.wasm.js')
    shutil.copy('test.js', 'test.wasm')
    self.run_browser('test.html', '/report_result?exit:0')

  def test_system(self):
    self.btest_exit('test_system.c')

  # Tests the hello_wasm_worker.c documentation example code.
  @also_with_minimal_runtime
  def test_wasm_worker_hello(self):
    self.btest_exit('wasm_worker/hello_wasm_worker.c', cflags=['-sWASM_WORKERS', '-sENVIRONMENT=web'])

  @requires_es6_workers
  def test_wasm_worker_hello_export_es6(self):
    self.btest_exit('wasm_worker/hello_wasm_worker.c', cflags=['-sWASM_WORKERS', '-sENVIRONMENT=web', '-sEXPORT_ES6'])

  def test_wasm_worker_hello_minimal_runtime_2(self):
    self.btest_exit('wasm_worker/hello_wasm_worker.c', cflags=['-sWASM_WORKERS', '-sMINIMAL_RUNTIME=2'])

  # Tests Wasm Workers build in Wasm2JS mode.
  @requires_wasm2js
  @also_with_minimal_runtime
  def test_wasm_worker_hello_wasm2js(self):
    self.btest_exit('wasm_worker/hello_wasm_worker.c', cflags=['-sWASM_WORKERS', '-sWASM=0'])

  # Tests the WASM_WORKERS=2 build mode, which embeds the Wasm Worker bootstrap JS script file to the main JS file.
  @also_with_minimal_runtime
  def test_wasm_worker_hello_embedded(self):
    self.btest_exit('wasm_worker/hello_wasm_worker.c', cflags=['-sWASM_WORKERS=2'])

  # Tests that it is possible to call emscripten_futex_wait() in Wasm Workers when pthreads
  # are also enabled.
  @parameterized({
    '': ([],),
    'pthread': (['-pthread'],),
  })
  def test_wasm_worker_futex_wait(self, args):
    self.btest_exit('wasm_worker/wasm_worker_futex_wait.c', cflags=['-sWASM_WORKERS=1', '-sASSERTIONS'] + args)

  # Tests Wasm Worker thread stack setup
  @also_with_minimal_runtime
  @parameterized({
    '0': (0,),
    '1': (1,),
    '2': (2,),
  })
  def test_wasm_worker_thread_stack(self, mode):
    self.btest('wasm_worker/thread_stack.c', expected='0', cflags=['-sWASM_WORKERS', f'-sSTACK_OVERFLOW_CHECK={mode}'])

  # Tests emscripten_malloc_wasm_worker() and emscripten_current_thread_is_wasm_worker() functions
  @also_with_minimal_runtime
  def test_wasm_worker_malloc(self):
    self.btest_exit('wasm_worker/malloc_wasm_worker.c', cflags=['-sWASM_WORKERS'])

  # Tests Wasm Worker+pthreads simultaneously
  @also_with_minimal_runtime
  def test_wasm_worker_and_pthreads(self):
    self.btest('wasm_worker/wasm_worker_and_pthread.c', expected='0', cflags=['-sWASM_WORKERS', '-pthread', '-sPTHREAD_POOL_SIZE=1'])

  # Tests emscripten_wasm_worker_self_id() function
  @also_with_minimal_runtime
  def test_wasm_worker_self_id(self):
    self.btest('wasm_worker/wasm_worker_self_id.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests direct Wasm Assembly .S file based TLS variables in Wasm Workers
  @also_with_minimal_runtime
  def test_wasm_worker_tls_wasm_assembly(self):
    self.btest('wasm_worker/wasm_worker_tls_wasm_assembly.c',
               expected='42', cflags=['-sWASM_WORKERS', test_file('wasm_worker/wasm_worker_tls_wasm_assembly.S')])

  # Tests C++11 keyword thread_local for TLS in Wasm Workers
  @also_with_minimal_runtime
  def test_wasm_worker_cpp11_thread_local(self):
    self.btest('wasm_worker/cpp11_thread_local.cpp', expected='42', cflags=['-sWASM_WORKERS'])

  # Tests C11 keyword _Thread_local for TLS in Wasm Workers
  @also_with_minimal_runtime
  def test_wasm_worker_c11__Thread_local(self):
    self.btest('wasm_worker/c11__Thread_local.c', expected='42', cflags=['-sWASM_WORKERS'])

  # Tests GCC specific extension keyword __thread for TLS in Wasm Workers
  @also_with_minimal_runtime
  def test_wasm_worker_gcc___thread(self):
    self.btest('wasm_worker/gcc___Thread.c', expected='42', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_wasm_worker_sleep()
  @also_with_minimal_runtime
  def test_wasm_worker_sleep(self):
    self.btest('wasm_worker/wasm_worker_sleep.c', expected='1', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_terminate_wasm_worker()
  @also_with_minimal_runtime
  def test_wasm_worker_terminate(self):
    self.btest_exit('wasm_worker/terminate_wasm_worker.c', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_terminate_all_wasm_workers()
  @also_with_minimal_runtime
  def test_wasm_worker_terminate_all(self):
    self.set_setting('WASM_WORKERS')
    # Test uses the dynCall library function in its EM_ASM code
    self.set_setting('DEFAULT_LIBRARY_FUNCS_TO_INCLUDE', ['$dynCall'])
    self.btest('wasm_worker/terminate_all_wasm_workers.c', expected='0')

  # Tests emscripten_wasm_worker_post_function_*() API
  @also_with_minimal_runtime
  def test_wasm_worker_post_function(self):
    self.btest('wasm_worker/post_function.c', expected='8', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_wasm_worker_post_function_*() API and EMSCRIPTEN_WASM_WORKER_ID_PARENT
  # to send a message back from Worker to its parent thread.
  @also_with_minimal_runtime
  def test_wasm_worker_post_function_to_main_thread(self):
    self.btest('wasm_worker/post_function_to_main_thread.c', expected='10', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_navigator_hardware_concurrency() and emscripten_atomics_is_lock_free()
  @also_with_minimal_runtime
  def test_wasm_worker_hardware_concurrency_is_lock_free(self):
    self.btest('wasm_worker/hardware_concurrency_is_lock_free.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_atomic_wait_u32() and emscripten_atomic_notify() functions.
  @also_with_minimal_runtime
  def test_wasm_worker_wait32_notify(self):
    self.btest('atomic/test_wait32_notify.c', expected='3', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_atomic_wait_u64() and emscripten_atomic_notify() functions.
  @also_with_minimal_runtime
  def test_wasm_worker_wait64_notify(self):
    self.btest('atomic/test_wait64_notify.c', expected='3', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_atomic_wait_async() function.
  @also_with_minimal_runtime
  def test_wasm_worker_wait_async(self):
    self.btest_exit('atomic/test_wait_async.c', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_atomic_cancel_wait_async() function.
  @also_with_minimal_runtime
  def test_wasm_worker_cancel_wait_async(self):
    self.btest('wasm_worker/cancel_wait_async.c', expected='1', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_atomic_cancel_all_wait_asyncs() function.
  @also_with_minimal_runtime
  def test_wasm_worker_cancel_all_wait_asyncs(self):
    self.btest('wasm_worker/cancel_all_wait_asyncs.c', expected='1', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_atomic_cancel_all_wait_asyncs_at_address() function.
  @also_with_minimal_runtime
  def test_wasm_worker_cancel_all_wait_asyncs_at_address(self):
    self.btest('wasm_worker/cancel_all_wait_asyncs_at_address.c', expected='1', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_lock_init(), emscripten_lock_waitinf_acquire() and emscripten_lock_release()
  @also_with_minimal_runtime
  def test_wasm_worker_lock_waitinf(self):
    self.btest('wasm_worker/lock_waitinf_acquire.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_lock_wait_acquire() and emscripten_lock_try_acquire() in Worker.
  @also_with_minimal_runtime
  def test_wasm_worker_lock_wait(self):
    self.btest('wasm_worker/lock_wait_acquire.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_lock_wait_acquire() between two Wasm Workers.
  @also_with_minimal_runtime
  def test_wasm_worker_lock_wait2(self):
    self.btest('wasm_worker/lock_wait_acquire2.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_lock_async_acquire() function.
  @also_with_minimal_runtime
  @flaky('https://github.com/emscripten-core/emscripten/issues/25270')
  def test_wasm_worker_lock_async_acquire(self):
    self.btest_exit('wasm_worker/lock_async_acquire.c', cflags=['--closure=1', '-sWASM_WORKERS'])

  # Tests emscripten_lock_async_acquire() function when lock is acquired both synchronously and asynchronously.
  @also_with_minimal_runtime
  def test_wasm_worker_lock_async_and_sync_acquire(self):
    self.btest_exit('wasm_worker/lock_async_and_sync_acquire.c', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_lock_busyspin_wait_acquire() in Worker and main thread.
  @also_with_minimal_runtime
  def test_wasm_worker_lock_busyspin_wait(self):
    self.btest('wasm_worker/lock_busyspin_wait_acquire.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_lock_busyspin_waitinf_acquire() in Worker and main thread.
  @also_with_minimal_runtime
  def test_wasm_worker_lock_busyspin_waitinf(self):
    self.btest('wasm_worker/lock_busyspin_waitinf_acquire.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests that proxied JS functions cannot be called from Wasm Workers
  @also_with_minimal_runtime
  def test_wasm_worker_no_proxied_js_functions(self):
    self.set_setting('WASM_WORKERS')
    self.set_setting('ASSERTIONS')
    # Test uses the dynCall library function in its EM_ASM code
    self.set_setting('DEFAULT_LIBRARY_FUNCS_TO_INCLUDE', ['$dynCall'])
    self.btest('wasm_worker/no_proxied_js_functions.c', expected='0',
               cflags=['--js-library', test_file('wasm_worker/no_proxied_js_functions.js')])

  # Tests emscripten_semaphore_init(), emscripten_semaphore_waitinf_acquire() and emscripten_semaphore_release()
  @also_with_minimal_runtime
  def test_wasm_worker_semaphore_waitinf_acquire(self):
    self.btest('wasm_worker/semaphore_waitinf_acquire.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_semaphore_wait_acquire()
  @also_with_minimal_runtime
  def test_wasm_worker_semaphore_wait_acquire(self):
    self.btest('wasm_worker/semaphore_wait_acquire.c', expected='0', cflags=['-sWASM_WORKERS'])

  # Tests emscripten_semaphore_try_acquire() on the main thread
  @also_with_minimal_runtime
  def test_wasm_worker_semaphore_try_acquire(self):
    self.btest_exit('wasm_worker/semaphore_try_acquire.c', cflags=['-sWASM_WORKERS'])

  @also_with_minimal_runtime
  def test_wasm_worker_condvar_waitinf(self):
    self.btest_exit('wasm_worker/condvar_waitinf.c', cflags=['-sWASM_WORKERS'])

  # Tests that calling any proxied function in a Wasm Worker will abort at runtime when ASSERTIONS are enabled.
  def test_wasm_worker_proxied_function(self):
    error_msg = "abort:Assertion failed: Attempted to call proxied function '_proxied_js_function' in a Wasm Worker, but in Wasm Worker enabled builds, proxied function architecture is not available!"
    # Test that program aborts in ASSERTIONS-enabled builds
    self.btest('wasm_worker/proxied_function.c', expected=error_msg, cflags=['--js-library', test_file('wasm_worker/proxied_function.js'), '-sWASM_WORKERS', '-sASSERTIONS'])
    # Test that code does not crash in ASSERTIONS-disabled builds
    self.btest('wasm_worker/proxied_function.c', expected='0', cflags=['--js-library', test_file('wasm_worker/proxied_function.js'), '-sWASM_WORKERS', '-sASSERTIONS=0'])

  @no_firefox('no 4GB support yet')
  @no_2gb('uses MAXIMUM_MEMORY')
  @no_4gb('uses MAXIMUM_MEMORY')
  def test_4gb(self):
    # TODO Convert to an actual browser test when it reaches stable.
    # For now, keep this in browser as this suite runs serially, which
    # means we don't compete for memory with anything else (and run it
    # at the very very end, to reduce the risk of it OOM-killing the
    # browser).

    # test that we can allocate in the 2-4GB range, if we enable growth and
    # set the max appropriately
    self.cflags += ['-O2', '-sALLOW_MEMORY_GROWTH', '-sMAXIMUM_MEMORY=4GB']
    self.do_run_in_out_file_test('browser/test_4GB.cpp')

  # Tests that emmalloc supports up to 4GB Wasm heaps.
  @no_firefox('no 4GB support yet')
  @requires_safari_version(170601, 'Assertion failed: emscripten_get_heap_size() == MAX_HEAP') # Fails in Safari 17.6 (17618.3.11.11.7, 17618)
  @no_4gb('uses MAXIMUM_MEMORY')
  def test_emmalloc_4gb(self):
    # For now, keep this in browser as this suite runs serially, which
    # means we don't compete for memory with anything else (and run it
    # at the very very end, to reduce the risk of it OOM-killing the
    # browser).
    self.btest_exit('test_mem_growth.c', cflags=['-sMALLOC=emmalloc', '-sABORTING_MALLOC=0', '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=4GB'])

  # Test that it is possible to malloc() a huge 3GB memory block in 4GB mode using emmalloc.
  # Also test emmalloc-memvalidate and emmalloc-memvalidate-verbose build configurations.
  @no_firefox('no 4GB support yet')
  @no_2gb('not enough space to run in this mode')
  @parameterized({
    '': (['-sMALLOC=emmalloc'],),
    'debug': (['-sMALLOC=emmalloc-debug'],),
    'memvalidate': (['-sMALLOC=emmalloc-memvalidate'],),
    'memvalidate_verbose': (['-sMALLOC=emmalloc-memvalidate-verbose'],),
  })
  def test_emmalloc_3gb(self, args):
    if self.is_4gb():
      self.set_setting('MAXIMUM_MEMORY', '8GB')
    else:
      self.set_setting('MAXIMUM_MEMORY', '4GB')
    self.btest_exit('alloc_3gb.c', cflags=['-sALLOW_MEMORY_GROWTH=1'] + args)

  # Test that it is possible to malloc() a huge 3GB memory block in 4GB mode using dlmalloc.
  @no_firefox('no 4GB support yet')
  @no_2gb('not enough space to run in this mode')
  def test_dlmalloc_3gb(self):
    if self.is_4gb():
      self.set_setting('MAXIMUM_MEMORY', '8GB')
    else:
      self.set_setting('MAXIMUM_MEMORY', '4GB')
    self.btest_exit('alloc_3gb.c', cflags=['-sMALLOC=dlmalloc', '-sALLOW_MEMORY_GROWTH=1'])

  @no_wasm64()
  @parameterized({
    # the fetch backend works even on the main thread: we proxy to a background
    # thread and busy-wait
    # this test requires one thread per fetch backend, so updates to the test
    # will require bumping this
    'main_thread': (['-sPTHREAD_POOL_SIZE=5'],),
    # using proxy_to_pthread also works, of course
    'proxy_to_pthread': (['-sPROXY_TO_PTHREAD', '-DPROXYING'],),
    # using BigInt support affects the ABI, and should not break things. (this
    # could be tested on either thread; do the main thread for simplicity)
    'bigint': (['-sPTHREAD_POOL_SIZE=5', '-sWASM_BIGINT'],),
  })
  def test_wasmfs_fetch_backend_threaded(self, args):
    create_file('data.dat', 'hello, fetch')
    create_file('small.dat', 'hello')
    create_file('test.txt', 'fetch 2')
    delete_dir('subdir')
    ensure_dir('subdir')
    create_file('subdir/backendfile', 'file 1')
    create_file('subdir/backendfile2', 'file 2')
    self.btest_exit('wasmfs/wasmfs_fetch.c',
                    cflags=['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD',
                            '-sFORCE_FILESYSTEM', '-lfetchfs.js',
                            '--js-library', test_file('wasmfs/wasmfs_fetch.js')] + args)

  @no_firefox('no OPFS support yet')
  @no_wasm64()
  @parameterized({
    '': (['-pthread', '-sPROXY_TO_PTHREAD'],),
    'jspi': (['-Wno-experimental', '-sJSPI'],),
    'jspi_wasm_bigint': (['-Wno-experimental', '-sJSPI', '-sWASM_BIGINT'],),
    'asyncify': (['-sASYNCIFY=1'],),
  })
  @no_safari('TODO: Fails with abort:Assertion failed: err == 0') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_wasmfs_opfs(self, args):
    if '-sJSPI' in args:
      self.require_jspi()
    test = test_file('wasmfs/wasmfs_opfs.c')
    args = ['-sWASMFS', '-O3'] + args
    self.btest_exit(test, cflags=args + ['-DWASMFS_SETUP'])
    self.btest_exit(test, cflags=args + ['-DWASMFS_RESUME'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_root_remount(self):
    self.btest_exit(
      'wasmfs/wasmfs_opfs_root_remount.c',
      cflags=['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_file_handle_cache(self):
    test = test_file('wasmfs/wasmfs_opfs_file_handle_cache.c')
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD',
                   '-sEXIT_RUNTIME', '-lopfs.js']
    create_file('file-handle-cache-pre.js', r'''
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'exit',
            status,
            type: 'wasmfs-opfs-file-handle-cache',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-sWASMFS_OPFS_TEST_FILE_HANDLE_CACHE=1', '--pre-js',
                     'file-handle-cache-pre.js', '-o',
                     'file-handle-cache.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kPrepared = 0;
        const kIdle = 1;
        const kFinished = 2;
        const kModuleTimeoutMs = 15000;
        const kTraceType = 'wasmfs-opfs-test-file-handle-cache';
        const kResultType = 'wasmfs-opfs-file-handle-cache';
        const phases = new Map();
        const phaseWaiters = new Map();
        const traceWaiters = new Set();
        const exitWaiters = new Map();
        const liveFileHandles = new Set();
        let acquires = 0;
        let releases = 0;
        let frame;
        const traceChannel = new BroadcastChannel(kTraceType);

        function waitForPhase(phase) {
          if (phases.has(phase)) {
            return Promise.resolve(phases.get(phase));
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              phaseWaiters.delete(phase);
              reject(new Error('timed out waiting for phase ' + phase));
            }, kModuleTimeoutMs);
            phaseWaiters.set(phase, {resolve, timeout});
          });
        }

        function waitForTrace(expected) {
          return new Promise((resolve, reject) => {
            const check = () => {
              if (acquires > expected || releases > expected) {
                traceWaiters.delete(check);
                clearTimeout(timeout);
                reject(new Error('unexpected file-handle trace count: ' +
                                 'acquires=' + acquires + ', releases=' +
                                 releases + ', expected=' + expected));
                return;
              }
              if (acquires == expected && releases == expected &&
                  liveFileHandles.size == 0) {
                traceWaiters.delete(check);
                clearTimeout(timeout);
                resolve();
              }
            };
            const timeout = setTimeout(() => {
              traceWaiters.delete(check);
              reject(new Error('timed out waiting for ' + expected +
                               ' balanced file-handle trace events; got ' +
                               acquires + ' acquires, ' + releases +
                               ' releases, and ' + liveFileHandles.size +
                               ' live handles'));
            }, kModuleTimeoutMs);
            traceWaiters.add(check);
            check();
          });
        }

        function waitForExit(target) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              exitWaiters.delete(target);
              reject(new Error('timed out waiting for clean module exit'));
            }, kModuleTimeoutMs);
            exitWaiters.set(target, {resolve, timeout});
          });
        }

        traceChannel.onmessage = (event) => {
          const trace = event.data;
          if (trace?.type != kTraceType || !Number.isInteger(trace.fileID) ||
              trace.fileID <= 0) {
            return;
          }
          if (trace.phase == 'acquire') {
            liveFileHandles.add(trace.fileID);
            ++acquires;
          } else if (trace.phase == 'release') {
            liveFileHandles.delete(trace.fileID);
            ++releases;
          } else {
            return;
          }
          for (const waiter of [...traceWaiters]) {
            waiter();
          }
        };

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kResultType) {
            return;
          }
          if (event.data.event == 'exit') {
            const waiter = exitWaiters.get(event.source);
            if (!waiter) {
              return;
            }
            exitWaiters.delete(event.source);
            clearTimeout(waiter.timeout);
            if (event.data.status != 0) {
              waiter.reject(new Error('module did not exit cleanly: status=' +
                                      event.data.status));
            } else {
              waiter.resolve();
            }
            return;
          }
          if (!Number.isInteger(event.data.phase) ||
              !Number.isInteger(event.data.error)) {
            return;
          }
          const phase = event.data.phase;
          const result = {error: event.data.error};
          phases.set(phase, result);
          const waiter = phaseWaiters.get(phase);
          if (waiter) {
            phaseWaiters.delete(phase);
            clearTimeout(waiter.timeout);
            waiter.resolve(result);
          }
        });

        (async () => {
          frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          frame.src = 'file-handle-cache.html';

          let result = await waitForPhase(kPrepared);
          if (result.error != 0) {
            throw new Error('test setup failed: errno=' + result.error);
          }

          frame.contentWindow.Module._wasmfs_opfs_file_handle_cache_start();
          result = await waitForPhase(kIdle);
          if (result.error != 0) {
            throw new Error('initial close failed: errno=' + result.error);
          }
          // The initial writable open/close must leave no strong idle
          // FileSystemFileHandle reference in the ProxyWorker.
          await waitForTrace(1);

          frame.contentWindow.Module
            ._wasmfs_opfs_file_handle_cache_continue();
          result = await waitForPhase(kFinished);
          if (result.error != 0) {
            throw new Error('lazy reacquire test failed: errno=' + result.error);
          }
          // One initial open plus stat, truncate, rename, reopen, final stat,
          // and the separate metadata probe each acquire and release exactly
          // one JS file-handle slot. The probe's fresh-wrapper rejected chmod
          // adds none.
          await waitForTrace(7);

          const moduleExit = waitForExit(frame.contentWindow);
          frame.contentWindow.Module
            ._wasmfs_opfs_file_handle_cache_shutdown();
          await moduleExit;
          traceChannel.close();
          frame.remove();
          reportResultToServer('0');
        })().catch((error) => {
          traceChannel.close();
          if (frame) {
            frame.remove();
          }
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=60)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_metadata_unsupported(self):
    self.btest_exit(
      'wasmfs/wasmfs_opfs_metadata_unsupported.c',
      cflags=['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_quota_write(self):
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    # First prove that the default-off setting leaves the pthread
    # SyncAccessHandle write path unchanged. The second build injects a
    # QuotaExceededError immediately before that native write.
    self.btest_exit(
      'wasmfs/wasmfs_opfs_quota_write.c',
      cflags=common_args)
    self.btest_exit(
      'wasmfs/wasmfs_opfs_quota_write.c',
      cflags=common_args + ['-DWASMFS_OPFS_QUOTA_WRITE_INJECTED',
                            '-sWASMFS_OPFS_TEST_QUOTA_WRITE=1'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_quota_truncate(self):
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    # First prove that the default-off setting leaves the pthread
    # SyncAccessHandle truncate path unchanged. The second build injects a
    # QuotaExceededError immediately before that native truncate.
    self.btest_exit(
      'wasmfs/wasmfs_opfs_quota_truncate.c',
      cflags=common_args)
    self.btest_exit(
      'wasmfs/wasmfs_opfs_quota_truncate.c',
      cflags=common_args + ['-DWASMFS_OPFS_QUOTA_TRUNCATE_INJECTED',
                            '-sWASMFS_OPFS_TEST_QUOTA_TRUNCATE=1'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_fallocate(self):
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    # The default build grows the file through an OPFS SyncAccessHandle. The
    # injected build fails before the native truncate, proving that
    # posix_fallocate propagates quota exhaustion instead of reporting success.
    self.btest_exit(
      'wasmfs/wasmfs_opfs_fallocate.c',
      cflags=common_args)
    self.btest_exit(
      'wasmfs/wasmfs_opfs_fallocate.c',
      cflags=common_args + ['-DWASMFS_OPFS_FALLOCATE_INJECTED',
                            '-sWASMFS_OPFS_TEST_QUOTA_TRUNCATE=1'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_positioned_io_range(self):
    self.btest_exit(
      'wasmfs/wasmfs_opfs_positioned_io_range.c',
      cflags=['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_open_truncate(self):
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    # A writable O_TRUNC must not return a descriptor if the access-handle
    # truncate fails. The injected build then remounts the same backend before
    # proving that the failed open released its browser-side access handle.
    self.btest_exit(
      'wasmfs/wasmfs_opfs_open_truncate.c',
      cflags=common_args)
    self.btest_exit(
      'wasmfs/wasmfs_opfs_open_truncate.c',
      cflags=common_args + ['-DWASMFS_OPFS_OPEN_TRUNCATE_INJECTED',
                            '-sWASMFS_OPFS_TEST_QUOTA_TRUNCATE=1'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_writable_truncate(self):
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    # The first build covers a healthy pathname truncate. The second injects
    # QuotaExceededError after createWritable succeeds, before its native
    # truncate, and verifies that the cleanup releases the writable lock.
    self.btest_exit(
      'wasmfs/wasmfs_opfs_writable_truncate.c',
      cflags=common_args)
    self.btest_exit(
      'wasmfs/wasmfs_opfs_writable_truncate.c',
      cflags=common_args + ['-DWASMFS_OPFS_WRITABLE_TRUNCATE_INJECTED',
                            '-sWASMFS_OPFS_TEST_QUOTA_WRITABLE_TRUNCATE=1'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_write_file(self):
    test = test_file('wasmfs/wasmfs_opfs_write_file.c')
    common_args = ['-sWASMFS', '-sFORCE_FILESYSTEM', '-pthread',
                   '-sPROXY_TO_PTHREAD', '-sEXIT_RUNTIME', '-lopfs.js']
    create_file('write-file-holder-pre.js', r'''
      // The C holder calls FS.writeFile from its runtime pthread. This hook
      // only confirms that its later orderly teardown did not abort.
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-write-file',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_WRITE_FILE_HOLDER', '--pre-js',
                     'write-file-holder-pre.js', '-o',
                     'write-file-normal-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-o', 'write-file-normal-contender.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_WRITE_FILE_HOLDER',
                     '-DWASMFS_OPFS_WRITE_FILE_QUOTA',
                     '-sWASMFS_OPFS_TEST_QUOTA_WRITE=1', '--pre-js',
                     'write-file-holder-pre.js', '-o',
                     'write-file-quota-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_WRITE_FILE_HOLDER',
                     '-DWASMFS_OPFS_WRITE_FILE_CLOSE_FAILURE',
                     '-sWASMFS_OPFS_TEST_QUOTA_WRITE=1',
                     '-sWASMFS_OPFS_TEST_CLOSE_FAILURE=1', '--pre-js',
                     'write-file-holder-pre.js', '-o',
                     'write-file-close-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_WRITE_FILE_CLOSE_FAILURE', '-o',
                     'write-file-close-contender.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kModuleTimeoutMs = 15000;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder shutdown'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-write-file') {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status != 0) {
              pending.reject(new Error(
                'holder did not exit cleanly: status=' + event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runScenario(name, holderPath, contenderPath) {
          const holder = await startModule(holderPath);
          if (holder.message.role != kHolder || holder.message.error != 0) {
            throw new Error(name + ' holder failed: role=' +
                            holder.message.role + ', errno=' +
                            holder.message.error);
          }

          // This is a distinct Wasm module and OPFS backend. It can acquire a
          // writer only if the holder's runtime-pthread FS.writeFile operation
          // closed its SyncAccessHandle (except in the close-failure scenario,
          // where the C contender explicitly expects EACCES).
          const contender = await startModule(contenderPath);
          if (contender.message.role != kContender ||
              contender.message.error != 0) {
            throw new Error(name + ' contender failed: role=' +
                            contender.message.role + ', errno=' +
                            contender.message.error);
          }

          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_write_file_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
        }

        (async () => {
          await runScenario('normal', 'write-file-normal-holder.html',
                            'write-file-normal-contender.html');
          await runScenario('quota failure', 'write-file-quota-holder.html',
                            'write-file-normal-contender.html');
          await runScenario('close failure', 'write-file-close-holder.html',
                            'write-file-close-contender.html');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=90)

  @no_firefox('no OPFS support yet')
  @no_safari('no Web Locks support yet')
  @no_wasm64()
  def test_wasmfs_opfs_profile_lease(self):
    self.compile_btest(
      'wasmfs/wasmfs_opfs_profile_lease.c',
      ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-sEXIT_RUNTIME',
       '-lopfs.js',
       '--pre-js',
       test_file('wasmfs/wasmfs_opfs_profile_lease_trace_pre.js'),
       '-o', 'lease.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kAcquired = 0;
        const kBusy = 1;
        const kReleaseDeadlineMs = 10000;
        const kReleaseRetryDelayMs = 100;
        const kModuleTimeoutMs = 15000;
        const pendingModules = new Map();
        const opfsTrace = new BroadcastChannel('wasmfs-opfs-profile-lease-trace');
        let opfsRootRequests = 0;
        let nextModuleId = 0;

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        async function expectOpfsRootRequests(expected, stage) {
          const deadline = Date.now() + 1000;
          while (Date.now() < deadline && opfsRootRequests < expected) {
            await delay(10);
          }
          if (opfsRootRequests != expected) {
            throw new Error(stage + ' made ' + opfsRootRequests +
                            ' OPFS root requests, expected ' + expected);
          }
        }

        opfsTrace.onmessage = (event) => {
          if (event.data?.type == 'opfs-root-request') {
            ++opfsRootRequests;
          }
        };

        function startModule() {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for leased module'));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = `lease.html?instance=${nextModuleId++}`;
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-profile-lease') {
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function acquireAfterOwnerShutdown() {
          const deadline = Date.now() + kReleaseDeadlineMs;
          while (Date.now() < deadline) {
            await delay(kReleaseRetryDelayMs);
            const candidate = await startModule();
            if (candidate.message.result == kAcquired) {
              return candidate;
            }
            candidate.frame.remove();
            if (candidate.message.result != kBusy) {
              throw new Error('lease recovery failed with result ' +
                              candidate.message.result + ', errno ' +
                              candidate.message.error);
            }
          }
          throw new Error(
            'lease was not released by the 10 second orderly shutdown deadline');
        }

        (async () => {
          const owner = await startModule();
          if (owner.message.result != kAcquired) {
            throw new Error('owner did not acquire lease: ' +
                            owner.message.result + ', errno ' +
                            owner.message.error);
          }
          await expectOpfsRootRequests(1, 'owner');

          const contender = await startModule();
          if (contender.message.result != kBusy) {
            throw new Error('contender did not get EBUSY: ' +
                            contender.message.result + ', errno ' +
                            contender.message.error);
          }
          contender.frame.remove();
          await expectOpfsRootRequests(1, 'contender');

          owner.frame.contentWindow.Module
            ._wasmfs_test_request_profile_lease_shutdown();
          const recovered = await acquireAfterOwnerShutdown();
          await expectOpfsRootRequests(2, 'recovered owner');
          owner.frame.remove();
          recovered.frame.remove();
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=60)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_unleased_record_locks(self):
    self.btest_exit(
      'wasmfs/wasmfs_opfs_unleased_record_locks.c',
      cflags=['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-sEXIT_RUNTIME',
              '-lopfs.js'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_terminal_drain(self):
    # This checks the narrow toolchain primitive only: an explicit terminal
    # drain releases a leased OPFS backend while its module remains alive, and
    # either kind of rejected AccessHandle close retains that lease. It does
    # not claim application, database, or browser-profile quiescence.
    test = test_file('wasmfs/wasmfs_opfs_terminal_drain.c')
    common_args = ['-sWASMFS', '-sFORCE_FILESYSTEM', '-pthread',
                   '-sPROXY_TO_PTHREAD',
                   '-sEXIT_RUNTIME', '-sALLOW_BLOCKING_ON_MAIN_THREAD=0',
                   '-lopfs.js']
    create_file('terminal-drain-holder-pre.js', r'''
      // This confirms orderly holder termination only after the parent has
      // observed its live-holder lease handoff/retention result.
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-terminal-drain',
          },
          window.location.origin);
      };
      Module['onAbort'] = (reason) => {
        window.parent.postMessage(
          {
            event: 'holder-abort',
            reason: String(reason),
            type: 'wasmfs-opfs-terminal-drain',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_TERMINAL_DRAIN_HOLDER',
                     '-sWASMFS_OPFS_PROFILE_DRAIN_TEST=1', '--pre-js',
                     'terminal-drain-holder-pre.js', '-o',
                     'terminal-drain-normal-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_TERMINAL_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_TERMINAL_DRAIN_CLOSE_DURING_DRAIN',
                     '-sWASMFS_OPFS_TEST_CLOSE_FAILURE=1', '--pre-js',
                     'terminal-drain-holder-pre.js', '-o',
                     'terminal-drain-close-during-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_TERMINAL_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_TERMINAL_DRAIN_CLOSE_BEFORE_DRAIN',
                     '-sWASMFS_OPFS_TEST_CLOSE_FAILURE=1', '--pre-js',
                     'terminal-drain-holder-pre.js', '-o',
                     'terminal-drain-close-before-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_TERMINAL_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_TERMINAL_DRAIN_RETIRE_FAILURE',
                     '-sWASMFS_OPFS_TEST_RETIRE_FAILURE=1', '--pre-js',
                     'terminal-drain-holder-pre.js', '-o',
                     'terminal-drain-retire-failure-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-o', 'terminal-drain-contender.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kReady = 0;
        const kBusy = 1;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();
        const retirementTrace = new BroadcastChannel(
          'wasmfs-opfs-profile-drain-retirement');
        let heartbeatStopped = false;
        let workerNotPooled = false;
        let retirementTraceError = null;

        retirementTrace.onmessage = (event) => {
          const trace = event.data;
          if (trace?.type != 'wasmfs-opfs-profile-drain-retirement') {
            return;
          }
          if (trace.phase == 'heartbeat-stopped') {
            heartbeatStopped = true;
          } else if (trace.phase == 'worker-not-pooled') {
            if (trace.inUnusedWorkers) {
              retirementTraceError =
                'retired terminal OPFS worker appeared in PThread.unusedWorkers';
            }
            workerNotPooled = true;
          } else if (trace.phase == 'heartbeat-tick' && heartbeatStopped) {
            retirementTraceError =
              'retired terminal OPFS worker emitted a stale heartbeat tick';
          }
        };

        async function waitForRetirementWitness() {
          const deadline = performance.now() + kModuleTimeoutMs;
          while (!retirementTraceError &&
                 (!heartbeatStopped || !workerNotPooled) &&
                 performance.now() < deadline) {
            await new Promise((resolve) => setTimeout(resolve, 10));
          }
          if (retirementTraceError) {
            throw new Error(retirementTraceError);
          }
          if (!heartbeatStopped || !workerNotPooled) {
            throw new Error('timed out waiting for terminal OPFS retirement ' +
                            'heartbeat/no-pool witnesses');
          }
          // The normal C fixture already created/joined a replacement pthread
          // before reporting ready. Keep its holder alive while a stale timer
          // would have an opportunity to fire after that deterministic churn.
          await new Promise((resolve) => setTimeout(resolve, 150));
          if (retirementTraceError) {
            throw new Error(retirementTraceError);
          }
        }

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function drainDetails(message) {
          return 'drain=' + message.drainResult +
                 ', terminal=' + message.terminalError +
                 ', data_file_states=' + message.dataFileStates +
                 ', libc_flush_failed=' + message.libcFlushFailed +
                 ', data_flush_failures=' + message.dataFlushFailures +
                 ', data_close_failures=' + message.dataCloseFailures +
                 ', backend_terminal_failures=' +
                 message.backendTerminalFailures;
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder exit'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-terminal-drain') {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status != 0) {
              pending.reject(new Error(
                'holder did not exit cleanly: status=' + event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          if (event.data.event == 'holder-abort') {
            const pendingExit = pendingHolderExits.get(event.source);
            if (pendingExit) {
              pendingHolderExits.delete(event.source);
              clearTimeout(pendingExit.timeout);
              pendingExit.reject(new Error(
                'holder aborted: ' + event.data.reason));
              return;
            }
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          if (event.data.event == 'holder-abort') {
            pendingModules.delete(event.source);
            clearTimeout(pending.timeout);
            pending.frame.remove();
            pending.reject(new Error('holder aborted: ' + event.data.reason));
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function startHolder(path, scenario) {
          // After a previous failed holder is disposed, browser-context Web
          // Locks cleanup is asynchronous. Retry only that cleanup boundary;
          // within each scenario the live-holder contender result is immediate.
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const holder = await startModule(path);
            if (holder.message.role != kHolder) {
              holder.frame.remove();
              throw new Error(scenario + ' holder reported role ' +
                              holder.message.role);
            }
            if (holder.message.result == kReady && holder.message.error == 0) {
              return holder;
            }
            holder.frame.remove();
            // `EBUSY` is classified in the C fixture. Do not assume the
            // numeric errno value in the JavaScript controller: Emscripten's
            // WASI errno ABI intentionally differs from native Linux's.
            if (holder.message.result != kBusy || holder.message.error == 0) {
              throw new Error(scenario + ' holder failed: result=' +
                              holder.message.result + ', errno=' +
                              holder.message.error + ', ' +
                              drainDetails(holder.message));
            }
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(scenario +
                          ' holder did not acquire a released profile lease');
        }

        async function runScenario(name,
                                   holderPath,
                                   expectBusy,
                                   expectRetiredWorker = false) {
          const holder = await startHolder(holderPath, name);
          if (holder.frame.contentWindow.Module
                ._wasmfs_opfs_terminal_drain_browser_main_attempt() != 0) {
            holder.frame.remove();
            throw new Error(name +
                            ' terminal drain did not reject browser-main call');
          }
          const contender = await startModule('terminal-drain-contender.html');
          if (contender.message.role != kContender) {
            contender.frame.remove();
            holder.frame.remove();
            throw new Error(name + ' contender reported role ' +
                            contender.message.role);
          }
          if (expectBusy) {
            if (contender.message.result != kBusy ||
                contender.message.error == 0) {
              contender.frame.remove();
              holder.frame.remove();
              throw new Error(name +
                              ' contender acquired despite retained lease: ' +
                              'result=' + contender.message.result +
                              ', errno=' + contender.message.error);
            }
          } else if (contender.message.result != kReady ||
                     contender.message.error != 0) {
            contender.frame.remove();
            holder.frame.remove();
            throw new Error(name +
                            ' contender did not acquire live-holder release: ' +
                              'result=' + contender.message.result +
                              ', errno=' + contender.message.error);
          }

          if (expectRetiredWorker) {
            await waitForRetirementWitness();
          }

          // The contender result above is observed before the holder's frame
          // is disposed. This distinguishes explicit terminal release from
          // browser-context cleanup at iframe removal.
          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_terminal_drain_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
        }

        async function runPostReleaseRetirementFailure() {
          const holder = await startHolder(
            'terminal-drain-retire-failure-holder.html',
            'post-release terminal retirement failure');
          if (holder.message.drainResult == 0 ||
              holder.message.terminalError == 0 ||
              holder.message.backendTerminalFailures != 1) {
            holder.frame.remove();
            throw new Error('post-release terminal failure was reported as a ' +
                            'safe handoff: ' + drainDetails(holder.message));
          }
          if (holder.frame.contentWindow.Module
                ._wasmfs_opfs_terminal_drain_browser_main_attempt() != 0) {
            holder.frame.remove();
            throw new Error('post-release terminal drain did not reject ' +
                            'browser-main call');
          }

          // The injected error follows the one-way Web Locks acknowledgement,
          // so a fresh contender must acquire while the failed holder remains
          // live. This proves consumers cannot infer success from release.
          const contender = await startModule('terminal-drain-contender.html');
          if (contender.message.role != kContender ||
              contender.message.result != kReady ||
              contender.message.error != 0) {
            contender.frame.remove();
            holder.frame.remove();
            throw new Error('post-release terminal failure contender did not ' +
                            'acquire released lock');
          }
          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_terminal_drain_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
        }

        (async () => {
          await runScenario('normal terminal drain',
                            'terminal-drain-normal-holder.html', false, true);
          await runScenario('close during terminal drain',
                            'terminal-drain-close-during-holder.html', true);
          await runScenario('close before terminal drain',
                            'terminal-drain-close-before-holder.html', true);
          await runPostReleaseRetirementFailure();
          retirementTrace.close();
          reportResultToServer('0');
        })().catch((error) => {
          retirementTrace.close();
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=90)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_namespace_recovery(self):
    # This is deliberately only a controlled selector-publication interruption
    # test for the opt-in logical namespace backend. It does not claim browser
    # crash, power-loss, database, or complete Chromium profile recovery.
    test = 'wasmfs/wasmfs_opfs_profile_namespace_recovery.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    terminal_drain_args = common_args + [
      '-sPTHREAD_POOL_SIZE=4',
      '-sEXIT_RUNTIME',
      '-sALLOW_BLOCKING_ON_MAIN_THREAD=0',
    ]
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_OWNER',
        '-o', 'owner.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT=1',
        '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT=1',
        '-o', 'mutator-before-selector.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_MUTATOR',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT=2',
        '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT=2',
        '-o', 'mutator-after-selector.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      terminal_drain_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_TERMINAL_DRAIN',
        '-o', 'verifier-terminal-drain.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_RECOVERY_VERIFIER',
        '-o', 'verifier-cleanup.html',
      ],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        // This coordinates only the test module's selector-publication hook.
        // It never reads or writes OPFS, and iframe disposal here is not a
        // claim about browser, renderer, power-loss, or database recovery.
        const kOwner = 0;
        const kMutator = 1;
        const kVerifier = 2;
        const kReady = 0;
        const kBusy = 1;
        const kStagedTree = 2;
        const kPublishedTree = 3;
        const kBeforeSelectorPublication = 1;
        const kAfterSelectorPublication = 2;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const kWitnessType = 'wasmfs-opfs-profile-namespace-recovery';
        const pendingModules = new Map();
        const pendingWitnesses = [];
        let witnessWaiter = undefined;
        const witnessChannel = new BroadcastChannel(kWitnessType);

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        async function startLeasedModule(path, role, description) {
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const candidate = await startModule(path);
            if (candidate.message.role != role) {
              candidate.frame.remove();
              throw new Error(description + ' reported role ' +
                              candidate.message.role + ', expected ' + role);
            }
            if (candidate.message.result != kBusy) {
              return candidate;
            }
            candidate.frame.remove();
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(description +
                          ' did not acquire a fresh OPFS profile lease');
        }

        function settleWitness(witness) {
          if (witnessWaiter && witnessWaiter.phase == witness.phase) {
            const {resolve, timeout} = witnessWaiter;
            witnessWaiter = undefined;
            clearTimeout(timeout);
            resolve(witness);
          } else {
            pendingWitnesses.push(witness);
          }
        }

        function waitForWitness(phase) {
          const index = pendingWitnesses.findIndex(
            (witness) => witness.phase == phase);
          if (index >= 0) {
            return Promise.resolve(pendingWitnesses.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              witnessWaiter = undefined;
              reject(new Error('timed out waiting for selector phase ' + phase));
            }, kModuleTimeoutMs);
            witnessWaiter = {phase, resolve, timeout};
          });
        }

        witnessChannel.onmessage = (event) => {
          if (event.data?.type == kWitnessType &&
              (event.data.phase == kBeforeSelectorPublication ||
               event.data.phase == kAfterSelectorPublication)) {
            settleWitness(event.data);
          }
        };

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType ||
              event.data.event != 'result') {
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runInterruptionCase(mutatorPath, phase, disposition) {
          const owner = await startLeasedModule('owner.html', kOwner, 'owner');
          if (owner.message.result != kReady || owner.message.error != 0) {
            owner.frame.remove();
            throw new Error('owner failed: result=' + owner.message.result +
                            ', errno=' + owner.message.error);
          }

          // The owner has already completed its result-bearing backend drain.
          // Removing its now-idle iframe neither establishes nor extends the
          // durability assertion made by this focused test.
          owner.frame.remove();

          const mutator = await startLeasedModule(
            mutatorPath, kMutator, 'selector phase ' + phase + ' mutator');
          if (mutator.message.result != kReady || mutator.message.error != 0) {
            mutator.frame.remove();
            throw new Error('selector phase ' + phase + ' mutator failed: ' +
                            'result=' + mutator.message.result + ', errno=' +
                            mutator.message.error);
          }

          await waitForWitness(phase);
          // The mutator is stopped in a test-only backend hook. Disposing this
          // document leaves the hook pending; the verifier must independently
          // acquire the leased namespace and observe exactly one full tree.
          mutator.frame.remove();

          const terminalVerifier = await startLeasedModule(
            'verifier-terminal-drain.html', kVerifier,
            'selector phase ' + phase + ' terminal verifier');
          if (terminalVerifier.message.result != disposition ||
              terminalVerifier.message.error != 0) {
            terminalVerifier.frame.remove();
            const expected = disposition == kStagedTree ?
              'complete staged tree' : 'complete published tree';
            throw new Error('selector phase ' + phase +
                            ' terminal verifier observed result=' +
                            terminalVerifier.message.result + ', errno=' +
                            terminalVerifier.message.error + ', expected ' +
                            expected);
          }

          // A second independently leased module must make the same exact
          // recovery selection while the terminal verifier's document remains
          // alive. That acquisition proves the explicit terminal drain—not
          // iframe teardown—released its Web Lock and container handle. This
          // verifier is the one that removes the fixture after its check.
          try {
            const cleanupVerifier = await startLeasedModule(
              'verifier-cleanup.html', kVerifier,
              'selector phase ' + phase + ' cleanup verifier');
            if (cleanupVerifier.message.result != disposition ||
                cleanupVerifier.message.error != 0) {
              cleanupVerifier.frame.remove();
              const expected = disposition == kStagedTree ?
                'complete staged tree' : 'complete published tree';
              throw new Error('selector phase ' + phase +
                              ' cleanup verifier observed result=' +
                              cleanupVerifier.message.result + ', errno=' +
                              cleanupVerifier.message.error + ', expected ' +
                              expected);
            }
            cleanupVerifier.frame.remove();
          } finally {
            terminalVerifier.frame.remove();
          }
        }

        (async () => {
          await runInterruptionCase(
            'mutator-before-selector.html', kBeforeSelectorPublication,
            kStagedTree);
          await runInterruptionCase(
            'mutator-after-selector.html', kAfterSelectorPublication,
            kPublishedTree);
          witnessChannel.close();
          reportResultToServer('0');
        })().catch((error) => {
          witnessChannel.close();
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_namespace_bootstrap_recovery(self):
    # Exercise every durable first-mount boundary. This is deliberately a
    # controlled outer-document interruption test for the experimental
    # namespace backend, not evidence of browser/power-loss/database recovery.
    test = 'wasmfs/wasmfs_opfs_profile_namespace_bootstrap_recovery.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile_nonce = f'{random.getrandbits(64):016x}'
    cases = [
      # A PREPARED recovery owns the initial mode until it flushes PUBLISHED.
      ('before-selector', 1, '0750'),
      ('after-selector', 2, '0750'),
      ('after-prepared-journal', 3, '0750'),
      # Once PUBLISHED is durable, a fresh mount must preserve its original
      # root mode rather than treating the profile as an unexposed bootstrap.
      ('after-published-journal', 4, '0711'),
      ('after-published-journal-mirror', 5, '0711'),
    ]
    case_literals = []
    for stem, phase, expected_mode in cases:
      profile = f'wasmfs_bootstrap_{profile_nonce}_{phase}'
      interrupter = f'bootstrap-{stem}-interrupter.html'
      verifier = f'bootstrap-{stem}-verifier.html'
      self.compile_btest(
        test,
        common_args + [
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INTERRUPTER',
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PROFILE_NAME=' + profile,
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INITIAL_MODE=0711',
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PHASE=' + str(phase),
          '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT=1',
          '-o', interrupter,
        ],
        reporting=Reporting.NONE)
      self.compile_btest(
        test,
        common_args + [
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_VERIFIER',
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_PROFILE_NAME=' + profile,
          # The fresh caller always asks for 0750. Before the first durable
          # PUBLISHED record it owns the initial mode; afterward it must
          # observe the interrupter's already-published 0711 instead.
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_INITIAL_MODE=0750',
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_BOOTSTRAP_EXPECTED_MODE=' +
          expected_mode,
          '-o', verifier,
        ],
        reporting=Reporting.NONE)
      case_literals.append(
        "{ expectedMode: %d, interrupter: '%s', phase: %d, verifier: '%s' }" %
        (int(expected_mode, 8), interrupter, phase, verifier))

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        // This page only schedules documented test hooks and disposes an
        // iframe. It never reads or writes OPFS itself.
        const kInterrupter = 0;
        const kVerifier = 1;
        const kResult = 0;
        const kInterruption = 1;
        const kReady = 0;
        const kBusy = 1;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const kWitnessType =
          'wasmfs-opfs-profile-namespace-bootstrap-recovery';
        const cases = [__BOOTSTRAP_CASES__];
        const pendingInterrupters = new Map();
        const pendingModules = new Map();
        const pendingWitnesses = [];
        let witnessWaiter;

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function startInterrupter(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          let rejectFailure;
          const failure = new Promise((resolve, reject) => {
            rejectFailure = reject;
          });
          pendingInterrupters.set(frame.contentWindow, {frame, rejectFailure});
          frame.src = path;
          return {failure, frame};
        }

        function disposeInterrupter(interrupter) {
          if (!interrupter) {
            return;
          }
          pendingInterrupters.delete(interrupter.frame.contentWindow);
          interrupter.frame.remove();
        }

        function settleWitness(witness) {
          if (witnessWaiter && witnessWaiter.phase == witness.mode) {
            const {resolve, timeout} = witnessWaiter;
            witnessWaiter = undefined;
            clearTimeout(timeout);
            resolve(witness);
          } else {
            pendingWitnesses.push(witness);
          }
        }

        function waitForWitness(phase) {
          const index = pendingWitnesses.findIndex(
            (witness) => witness.mode == phase);
          if (index >= 0) {
            return Promise.resolve(pendingWitnesses.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              witnessWaiter = undefined;
              reject(new Error('timed out waiting for bootstrap phase ' + phase));
            }, kModuleTimeoutMs);
            witnessWaiter = {phase, resolve, timeout};
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType) {
            return;
          }
          if (event.data.event == kInterruption &&
              event.data.role == kInterrupter &&
              event.data.result == kReady && event.data.error == 0) {
            settleWitness(event.data);
            return;
          }
          if (event.data.event != kResult) {
            return;
          }
          const interrupter = pendingInterrupters.get(event.source);
          if (interrupter) {
            pendingInterrupters.delete(event.source);
            interrupter.rejectFailure(new Error(
              'bootstrap interrupter returned before its hook: result=' +
              event.data.result + ', errno=' + event.data.error));
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function startLeasedVerifier(path, description) {
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const candidate = await startModule(path);
            if (candidate.message.role != kVerifier) {
              candidate.frame.remove();
              throw new Error(description + ' reported role ' +
                              candidate.message.role + ', expected verifier');
            }
            if (candidate.message.result != kBusy) {
              return candidate;
            }
            candidate.frame.remove();
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(description + ' did not acquire a fresh OPFS lease');
        }

        async function runCase(testCase) {
          let interrupter;
          let verifier;
          try {
            const witness = waitForWitness(testCase.phase);
            interrupter = startInterrupter(testCase.interrupter);
            await Promise.race([witness, interrupter.failure]);

            // The only interruption is teardown of the holder document after
            // its native test hook has reported the durable boundary.
            disposeInterrupter(interrupter);
            interrupter = undefined;

            verifier = await startLeasedVerifier(
              testCase.verifier, 'bootstrap phase ' + testCase.phase);
            if (verifier.message.result != kReady ||
                verifier.message.error != 0 ||
                verifier.message.mode != testCase.expectedMode) {
              throw new Error('bootstrap phase ' + testCase.phase +
                              ' recovery failed: result=' +
                              verifier.message.result + ', errno=' +
                              verifier.message.error + ', mode=' +
                              verifier.message.mode + ', expected mode=' +
                              testCase.expectedMode);
            }
          } finally {
            verifier?.frame.remove();
            disposeInterrupter(interrupter);
          }
        }

        (async () => {
          for (const testCase of cases) {
            await runCase(testCase);
          }
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    '''.replace('__BOOTSTRAP_CASES__', ', '.join(case_literals)))
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_namespace_empty_drain(self):
    # A namespace factory durably writes a PREPARED journal before callers
    # mount its logical root. This test keeps a successfully drained iframe
    # alive while a fresh module publishes the same profile, so context
    # teardown cannot hide a leaked Web Lock or a stranded bootstrap record.
    test = 'wasmfs/wasmfs_opfs_profile_namespace_empty_drain.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile_nonce = f'{random.getrandbits(64):016x}'
    scoped_profile = f'wasmfs_empty_scoped_{profile_nonce}'
    terminal_profile = f'wasmfs_empty_terminal_{profile_nonce}'
    terminal_drain_args = common_args + [
      '-sPTHREAD_POOL_SIZE=4',
      '-sEXIT_RUNTIME',
      '-sALLOW_BLOCKING_ON_MAIN_THREAD=0',
    ]
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_DRAIN',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_PROFILE_NAME=' +
        scoped_profile,
        '-o', 'empty-scoped.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_FRESH',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_PROFILE_NAME=' +
        scoped_profile,
        '-o', 'fresh-after-scoped.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      terminal_drain_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_DRAIN',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_TERMINAL',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_PROFILE_NAME=' +
        terminal_profile,
        '-o', 'empty-terminal.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_FRESH',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_EMPTY_PROFILE_NAME=' +
        terminal_profile,
        '-o', 'fresh-after-terminal.html',
      ],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kDrain = 0;
        const kFresh = 1;
        const kReady = 0;
        const kBusy = 1;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const kWitnessType = 'wasmfs-opfs-profile-namespace-empty-drain';
        const pendingModules = new Map();

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        async function startLeasedModule(path, role, description) {
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const candidate = await startModule(path);
            if (candidate.message.role != role) {
              candidate.frame.remove();
              throw new Error(description + ' reported role ' +
                              candidate.message.role + ', expected ' + role);
            }
            if (candidate.message.result != kBusy) {
              return candidate;
            }
            candidate.frame.remove();
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(description +
                          ' did not acquire a fresh OPFS profile lease');
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType ||
              event.data.event != 'result') {
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runCase(drainPath, freshPath, description) {
          const drain = await startLeasedModule(drainPath, kDrain, description);
          if (drain.message.result != kReady || drain.message.error != 0) {
            drain.frame.remove();
            throw new Error(description + ' drain failed: result=' +
                            drain.message.result + ', errno=' +
                            drain.message.error);
          }

          // Keep the drained iframe alive. The fresh mount therefore proves
          // the drain itself released its Web Lock and preserved a usable
          // PREPARED journal, rather than relying on document destruction.
          try {
            const fresh = await startLeasedModule(
              freshPath, kFresh, description + ' fresh mount');
            if (fresh.message.result != kReady || fresh.message.error != 0) {
              fresh.frame.remove();
              throw new Error(description + ' fresh mount failed: result=' +
                              fresh.message.result + ', errno=' +
                              fresh.message.error);
            }
            fresh.frame.remove();
          } finally {
            drain.frame.remove();
          }
        }

        (async () => {
          await runCase('empty-scoped.html', 'fresh-after-scoped.html',
                        'scoped empty-container drain');
          await runCase('empty-terminal.html', 'fresh-after-terminal.html',
                        'terminal empty-container drain');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_namespace_initialisation_failure(self):
    # Exercise factory failure ownership separately from ordinary namespace
    # recovery. The test-only variations discard a completed initialization
    # acknowledgement; they must retain a terminally visible tombstone. The
    # ordinary fresh modules then prove that private bootstrap debris can be
    # recovered only after the holder document has gone away.
    test = 'wasmfs/wasmfs_opfs_profile_namespace_initialisation_failure.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile_nonce = f'{random.getrandbits(64):016x}'
    profiles = {
      'root': f'wasmfs_init_root_{profile_nonce}',
      'lookup': f'wasmfs_init_lookup_{profile_nonce}',
      'insert': f'wasmfs_init_insert_{profile_nonce}',
      'known': f'wasmfs_init_known_{profile_nonce}',
    }

    for name, phase in [('root', 1), ('lookup', 2), ('insert', 3)]:
      self.compile_btest(
        test,
        common_args + [
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_TOMBSTONE',
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_PROFILE_NAME=' +
          profiles[name],
          '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE=' +
          str(phase),
          '-o', name + '-tombstone.html',
        ],
        reporting=Reporting.NONE)
      self.compile_btest(
        test,
        common_args + [
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_FRESH',
          '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_PROFILE_NAME=' +
          profiles[name],
          '-o', name + '-fresh.html',
        ],
        reporting=Reporting.NONE)

    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_KNOWN',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_PROFILE_NAME=' +
        profiles['known'],
        '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INITIALISATION_FAILURE=4',
        '-o', 'known-failure.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_FRESH',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_INIT_FAILURE_PROFILE_NAME=' +
        profiles['known'],
        '-o', 'known-fresh.html',
      ],
      reporting=Reporting.NONE)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kTombstone = 0;
        const kKnownFailure = 1;
        const kFresh = 2;
        const kReady = 0;
        const kBusy = 1;
        const kTombstoned = 2;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const kWitnessType =
          'wasmfs-opfs-profile-namespace-initialisation-failure';
        const pendingModules = new Map();

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        async function startLeasedModule(path, role, description) {
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const candidate = await startModule(path);
            if (candidate.message.role != role) {
              candidate.frame.remove();
              throw new Error(description + ' reported role ' +
                              candidate.message.role + ', expected ' + role);
            }
            if (candidate.message.result != kBusy) {
              return candidate;
            }
            candidate.frame.remove();
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(description +
                          ' did not acquire a fresh OPFS profile lease');
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType || event.data.event != 'result') {
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runTombstoneCase(holderPath, freshPath, description) {
          const holder = await startLeasedModule(
            holderPath, kTombstone, description + ' holder');
          if (holder.message.result != kTombstoned ||
              holder.message.error != 0) {
            holder.frame.remove();
            throw new Error(description + ' was not terminally fail-closed: ' +
                            'result=' + holder.message.result + ', errno=' +
                            holder.message.error);
          }
          // A second document must be blocked by the real browser Web Lock,
          // not merely by the failed factory's in-process reservation. Do one
          // no-retry contender while the tombstone remains alive, then remove
          // the holder and permit the ordinary retrying recovery path.
          try {
            const blocked = await startModule(freshPath);
            if (blocked.message.role != kFresh ||
                blocked.message.result != kBusy) {
              blocked.frame.remove();
              throw new Error(description + ' did not hold the browser lease: ' +
                              'result=' + blocked.message.result + ', errno=' +
                              blocked.message.error + ', stage=' +
                              blocked.message.stage);
            }
            blocked.frame.remove();
          } finally {
            holder.frame.remove();
          }
          const fresh = await startLeasedModule(
            freshPath, kFresh, description + ' fresh recovery');
          if (fresh.message.result != kReady || fresh.message.error != 0) {
            fresh.frame.remove();
            throw new Error(description + ' fresh recovery failed: result=' +
                            fresh.message.result + ', errno=' +
                            fresh.message.error + ', stage=' +
                            fresh.message.stage);
          }
          fresh.frame.remove();
        }

        async function runKnownFailureCase() {
          const holder = await startLeasedModule(
            'known-failure.html', kKnownFailure, 'known factory failure');
          if (holder.message.result != kReady || holder.message.error != 0) {
            holder.frame.remove();
            throw new Error('known factory failure did not cleanly hand off: ' +
                            'result=' + holder.message.result + ', errno=' +
                            holder.message.error);
          }
          // Keep this document alive: fresh acquisition proves the confirmed
          // remove/release path itself, not document teardown.
          try {
            const fresh = await startLeasedModule(
              'known-fresh.html', kFresh, 'known failure fresh recovery');
            if (fresh.message.result != kReady || fresh.message.error != 0) {
              fresh.frame.remove();
              throw new Error('known failure fresh recovery failed: result=' +
                              fresh.message.result + ', errno=' +
                              fresh.message.error);
            }
            fresh.frame.remove();
          } finally {
            holder.frame.remove();
          }
        }

        (async () => {
          await runTombstoneCase(
            'root-tombstone.html', 'root-fresh.html',
            'root initialization acknowledgement');
          await runTombstoneCase(
            'lookup-tombstone.html', 'lookup-fresh.html',
            'published-name lookup acknowledgement');
          await runTombstoneCase(
            'insert-tombstone.html', 'insert-fresh.html',
            'bootstrap insert acknowledgement');
          await runKnownFailureCase();
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_namespace_profile_name_isolation(self):
    # Keep the first profile's PREPARED bootstrap artifacts live while a
    # second profile whose logical name has the old bootstrap suffix
    # initializes. A random stem avoids persistent OPFS state from an earlier
    # browser test run while preserving the exact `collision` /
    # `collision.bootstrap` relationship.
    test = 'wasmfs/wasmfs_opfs_profile_namespace_profile_name_isolation.c'
    common_args = [
      '-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-sPTHREAD_POOL_SIZE=4',
      '-lopfs.js',
    ]
    profile_nonce = f'{random.getrandbits(64):016x}'
    profiles = {
      'collision': f'wasmfs_collision_{profile_nonce}',
      'collision.bootstrap': f'wasmfs_collision_{profile_nonce}.bootstrap',
    }

    def profile_args(profile, sentinel, role):
      return [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PROFILE_NAME=' + profile,
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_SENTINEL=' + sentinel,
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PROFILE_ROLE=' + str(role),
      ]

    self.compile_btest(
      test,
      common_args + profile_args(profiles['collision'], 'first_sentinel', 0) + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER',
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_PAUSE_BOOTSTRAP',
        '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_INTERRUPT=1',
        '-o', 'collision-holder.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args +
      profile_args(profiles['collision.bootstrap'], 'second_sentinel', 1) + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_HOLDER',
        '-o', 'collision-bootstrap-holder.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + profile_args(profiles['collision'], 'first_sentinel', 0) + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_VERIFIER',
        '-o', 'collision-verifier.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args +
      profile_args(profiles['collision.bootstrap'], 'second_sentinel', 1) + [
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_ISOLATION_VERIFIER',
        '-o', 'collision-bootstrap-verifier.html',
      ],
      reporting=Reporting.NONE)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kCollision = 0;
        const kCollisionBootstrap = 1;
        const kBootstrapReady = 0;
        const kReady = 1;
        const kDrained = 2;
        const kVerified = 3;
        const kModuleTimeoutMs = 20000;
        const kWitnessType =
          'wasmfs-opfs-profile-namespace-profile-name-isolation';
        const liveModules = new Map();

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {events: [], frame, path, waiters: new Map()};
          liveModules.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function waitForEvent(module, expectedEvent, description) {
          const index = module.events.findIndex(
            (message) => message.event == expectedEvent);
          if (index >= 0) {
            return Promise.resolve(module.events.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              module.waiters.delete(expectedEvent);
              reject(new Error('timed out waiting for ' + description));
            }, kModuleTimeoutMs);
            module.waiters.set(expectedEvent, {resolve, timeout});
          });
        }

        async function expectEvent(module, expectedRole, expectedEvent,
                                   description) {
          const message = await waitForEvent(module, expectedEvent, description);
          if (message.role != expectedRole || message.error != 0) {
            throw new Error(description + ' failed: role=' + message.role +
                            ', event=' + message.event + ', errno=' +
                            message.error);
          }
          return message;
        }

        function callExport(module, name, description) {
          const exported = module.frame.contentWindow.Module?.[name];
          if (typeof exported != 'function') {
            throw new Error(description + ' is missing export ' + name);
          }
          exported();
        }

        function disposeModule(module) {
          if (!module) {
            return;
          }
          for (const waiter of module.waiters.values()) {
            clearTimeout(waiter.timeout);
          }
          module.waiters.clear();
          liveModules.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType) {
            return;
          }
          const module = liveModules.get(event.source);
          if (!module) {
            return;
          }
          const waiter = module.waiters.get(event.data.event);
          if (!waiter) {
            module.events.push(event.data);
            return;
          }
          module.waiters.delete(event.data.event);
          clearTimeout(waiter.timeout);
          waiter.resolve(event.data);
        });

        (async () => {
          let firstHolder;
          let secondHolder;
          let firstVerifier;
          let secondVerifier;
          try {
            firstHolder = launchModule('collision-holder.html');
            await expectEvent(firstHolder, kCollision, kBootstrapReady,
                              'collision bootstrap staging');

            // Both profile leases remain live from here until their explicit
            // drains. If physical names are suffix-derived, this second
            // factory sees one of the first profile's bootstrap artifacts as
            // its canonical object and cannot produce this ready witness.
            secondHolder = launchModule('collision-bootstrap-holder.html');
            await expectEvent(secondHolder, kCollisionBootstrap, kReady,
                              'collision.bootstrap holder');

            callExport(firstHolder,
                       '_wasmfs_opfs_profile_namespace_isolation_resume_bootstrap',
                       'collision holder');
            await expectEvent(firstHolder, kCollision, kReady,
                              'collision holder');

            // Keep both holder documents alive while their result-bearing
            // drains run. A later verifier must acquire after the explicit
            // release, not after iframe teardown.
            callExport(firstHolder,
                       '_wasmfs_opfs_profile_namespace_isolation_request_drain',
                       'collision holder');
            callExport(secondHolder,
                       '_wasmfs_opfs_profile_namespace_isolation_request_drain',
                       'collision.bootstrap holder');
            await expectEvent(firstHolder, kCollision, kDrained,
                              'collision holder scoped drain');
            await expectEvent(secondHolder, kCollisionBootstrap, kDrained,
                              'collision.bootstrap holder scoped drain');

            firstVerifier = launchModule('collision-verifier.html');
            secondVerifier = launchModule('collision-bootstrap-verifier.html');
            await expectEvent(firstVerifier, kCollision, kVerified,
                              'collision reopen verifier');
            await expectEvent(secondVerifier, kCollisionBootstrap, kVerified,
                              'collision.bootstrap reopen verifier');
            reportResultToServer('0');
          } finally {
            disposeModule(secondVerifier);
            disposeModule(firstVerifier);
            disposeModule(secondHolder);
            disposeModule(firstHolder);
          }
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_namespace_journal_corruption(self):
    # A completed PUBLISHED namespace has two permanent journal witnesses.
    # Simulate loss of either successfully-read witness in a fresh module and
    # require EIO, then reopen normally to prove the failure did not reset the
    # established profile. This deliberately remains a native parser test,
    # not raw-OPFS mutation or browser/power-loss/database recovery evidence.
    test = 'wasmfs/wasmfs_opfs_profile_namespace_journal_corruption.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = f'wasmfs_journal_corruption_{random.getrandbits(64):016x}'
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_PROFILE_NAME=' + profile)

    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_OWNER',
        '-o', 'journal-owner.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_CORRUPTOR',
        '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION=1',
        '-o', 'journal-corrupt-slot-zero.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_CORRUPTOR',
        '-sWASMFS_OPFS_PROFILE_NAMESPACE_TEST_JOURNAL_CORRUPTION=2',
        '-o', 'journal-corrupt-slot-one.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_NAMESPACE_JOURNAL_VERIFIER',
        '-o', 'journal-verifier.html',
      ],
      reporting=Reporting.NONE)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kOwner = 0;
        const kCorruptor = 1;
        const kVerifier = 2;
        const kModuleTimeoutMs = 20000;
        const kWitnessType =
          'wasmfs-opfs-profile-namespace-journal-corruption';
        const pendingModules = new Map();

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType) {
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function expectSuccess(path, role, description) {
          const module = await startModule(path);
          try {
            if (module.message.role != role || module.message.error != 0) {
              throw new Error(description + ' failed: role=' +
                              module.message.role + ', errno=' +
                              module.message.error);
            }
          } finally {
            module.frame.remove();
          }
        }

        (async () => {
          await expectSuccess('journal-owner.html', kOwner,
                              'published namespace owner');
          await expectSuccess('journal-corrupt-slot-zero.html', kCorruptor,
                              'slot-zero journal corruption rejection');
          await expectSuccess('journal-corrupt-slot-one.html', kCorruptor,
                              'slot-one journal corruption rejection');
          await expectSuccess('journal-verifier.html', kVerifier,
                              'normal published namespace verifier');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v2_control(self):
    # The V2 control primitive is intentionally smaller than the namespace
    # backend: it commits one opaque integer root through fixed regular OPFS
    # files. These fresh-iframe tests prove bootstrap partial-set rejection,
    # old-root selection before CLEAN quorum, new-root selection after it, and
    # fail-closed selected-control parser behavior. They also check that the
    # control backend cannot be mounted and that an ordinary owned backend
    # rejects V2 calls. They do not claim browser/power-loss/directory/database
    # or complete Chromium profile recovery.
    test = 'wasmfs/wasmfs_opfs_profile_log_v2_control.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    new_root = '0x4c6f675632ULL'
    before_profile = f'wasmfs_profile_log_v2_before_{random.getrandbits(64):016x}'
    after_profile = f'wasmfs_profile_log_v2_after_{random.getrandbits(64):016x}'
    bootstrap_profile = f'wasmfs_profile_log_v2_bootstrap_{random.getrandbits(64):016x}'
    corrupt_profile = f'wasmfs_profile_log_v2_corrupt_{random.getrandbits(64):016x}'

    def profile_arg(profile):
      return '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_PROFILE_NAME=' + profile

    def compile_role(output, profile, role_args, extra_args=None):
      self.compile_btest(
        test,
        common_args + [profile_arg(profile)] + role_args +
          (extra_args or []) + ['-o', output],
        reporting=Reporting.NONE)

    compile_role('v2-before-owner.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER'])
    compile_role('v2-before-mutator.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_MUTATOR',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT_PHASE=1'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT=1'])
    compile_role('v2-before-verifier.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT=0ULL',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECT_COMMIT_REJECTION'])

    compile_role('v2-after-owner.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER'])
    compile_role('v2-after-mutator.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_MUTATOR',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT_PHASE=2'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT=1'])
    compile_role('v2-after-verifier.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT=' + new_root])

    compile_role('v2-bootstrap-owner.html', bootstrap_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT_PHASE=0'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V2_TEST_INTERRUPT=1'])
    compile_role('v2-bootstrap-corruptor.html', bootstrap_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_CORRUPTOR'])

    compile_role('v2-corrupt-owner.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_OWNER'])
    compile_role('v2-corrupt-mutator.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_MUTATOR'])
    compile_role('v2-corrupt-phase.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_CORRUPTOR'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION=1'])
    compile_role('v2-corrupt-descriptor.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_CORRUPTOR'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V2_TEST_SELECTED_CONTROL_CORRUPTION=2'])
    compile_role('v2-corrupt-verifier.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V2_TEST_EXPECTED_ROOT=' + new_root])

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kOwner = 0;
        const kMutator = 1;
        const kVerifier = 2;
        const kCorruptor = 3;
        const kReady = 0;
        const kCorruptionRejected = 1;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v2-control';
        const kEventTimeoutMs = 20000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, predicate, description) {
          const index = module.events.findIndex(predicate);
          if (index >= 0) {
            return Promise.resolve(module.events.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const waiter = module.waiters.findIndex(
                (candidate) => candidate.resolve === resolve);
              if (waiter >= 0) {
                module.waiters.splice(waiter, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({predicate, resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          const waiter = module.waiters.findIndex(
            (candidate) => candidate.predicate(event.data));
          if (waiter >= 0) {
            const candidate = module.waiters.splice(waiter, 1)[0];
            clearTimeout(candidate.timeout);
            candidate.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runResult(path, role, result, description) {
          const module = launchModule(path);
          try {
            const message = await waitFor(
              module, (event) => event.event === 'result', description);
            if (message.role !== role || message.result !== result ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
          } finally {
            disposeModule(module);
          }
        }

        async function runVerifierAfterRelease(path, expected, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(
                module, (event) => event.event === 'result', description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== kVerifier || message.result !== kReady ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error + ', expected=' + expected);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function runInterruptedScenario(owner, mutator, verifier,
                                               phase, expected, description) {
          await runResult(owner, kOwner, kReady, description + ' owner');
          const module = launchModule(mutator);
          try {
            const interruption = await waitFor(
              module,
              (event) => event.event === 'interrupt' && event.phase === phase,
              description + ' interruption');
            if (interruption.phase !== phase) {
              throw new Error(description + ' reported wrong interruption');
            }
          } finally {
            // Disposing this fresh document is the deliberate controlled
            // interruption. The parent never touches OPFS directly.
            disposeModule(module);
          }
          await runVerifierAfterRelease(verifier, expected,
                                        description + ' fresh verifier');
        }

        async function runCorruptorAfterRelease(path, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(
                module, (event) => event.event === 'result', description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== kCorruptor ||
                message.result !== kCorruptionRejected ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function runBootstrapInterruption(owner, corruptor) {
          const module = launchModule(owner);
          try {
            const interruption = await waitFor(
              module,
              (event) => event.event === 'interrupt' && event.phase === 0,
              'first bootstrap witness interruption');
            if (interruption.phase !== 0) {
              throw new Error('bootstrap interruption reported wrong witness');
            }
          } finally {
            // The incomplete fixed-file set remains in OPFS. The next factory
            // must fail closed rather than recreate or adopt it.
            disposeModule(module);
          }
          await runCorruptorAfterRelease(
            corruptor, 'partial bootstrap fresh factory rejection');
        }

        (async () => {
          await runBootstrapInterruption(
            'v2-bootstrap-owner.html', 'v2-bootstrap-corruptor.html');
          await runInterruptedScenario(
            'v2-before-owner.html', 'v2-before-mutator.html',
            'v2-before-verifier.html', 1, 0,
            'old root before CLEAN quorum');
          await runInterruptedScenario(
            'v2-after-owner.html', 'v2-after-mutator.html',
            'v2-after-verifier.html', 2, 0x4c6f675632,
            'new root after CLEAN quorum');
          await runResult('v2-corrupt-owner.html', kOwner, kReady,
                          'selected-control owner');
          await runResult('v2-corrupt-mutator.html', kMutator, kReady,
                          'selected-control update');
          await runResult('v2-corrupt-phase.html', kCorruptor,
                          kCorruptionRejected,
                          'selected phase corruption rejection');
          await runResult('v2-corrupt-descriptor.html', kCorruptor,
                          kCorruptionRejected,
                          'selected descriptor corruption rejection');
          await runVerifierAfterRelease('v2-corrupt-verifier.html',
                                        0x4c6f675632,
                                        'normal verifier after rejection');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v3_data(self):
    # V3 is deliberately a single non-mountable DataFile projection. These
    # fresh-iframe tests exercise its fixed bootstrap/control/arena layout:
    # partial-bootstrap rejection, old-image selection before the CLEAN
    # quorum, new-image selection after it, read-only g/g+1 recovery, and
    # selected control/manifest parser rejection. They do not claim physical
    # crash, namespace, directory, database, or Chromium profile recovery.
    test = 'wasmfs/wasmfs_opfs_profile_log_v3_data.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    before_profile = f'wasmfs_profile_log_v3_before_{random.getrandbits(64):016x}'
    after_profile = f'wasmfs_profile_log_v3_after_{random.getrandbits(64):016x}'
    bootstrap_profile = f'wasmfs_profile_log_v3_bootstrap_{random.getrandbits(64):016x}'
    corrupt_profile = f'wasmfs_profile_log_v3_corrupt_{random.getrandbits(64):016x}'
    resize_profile = f'wasmfs_profile_log_v3_resize_{random.getrandbits(64):016x}'
    forced_error_profile = f'wasmfs_profile_log_v3_forced_error_{random.getrandbits(64):016x}'

    def profile_arg(profile):
      return '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_PROFILE_NAME=' + profile

    def compile_role(output, profile, role_args, extra_args=None):
      self.compile_btest(
        test,
        common_args + [profile_arg(profile)] + role_args +
          (extra_args or []) + ['-o', output],
        reporting=Reporting.NONE)

    compile_role('v3-before-owner.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER'])
    compile_role('v3-before-mutator.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_MUTATOR',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT_PHASE=1'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT=1'])
    compile_role('v3-before-verifier.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_COMMIT_REJECTION'])

    compile_role('v3-after-owner.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER'])
    compile_role('v3-after-mutator.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_MUTATOR',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT_PHASE=2'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT=1'])
    compile_role('v3-after-verifier.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_NEW'])

    compile_role('v3-bootstrap-owner.html', bootstrap_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT_PHASE=0'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V3_TEST_INTERRUPT=1'])
    compile_role('v3-bootstrap-corruptor.html', bootstrap_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_CORRUPTOR'])

    compile_role('v3-corrupt-owner.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER'])
    compile_role('v3-corrupt-mutator.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_MUTATOR'])
    for selector, name in ((1, 'phase'), (2, 'descriptor'), (3, 'manifest')):
      compile_role(f'v3-corrupt-{name}.html', corrupt_profile,
                   ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_CORRUPTOR'],
                   ['-sWASMFS_OPFS_PROFILE_LOG_V3_TEST_SELECTED_CORRUPTION=' +
                    str(selector)])
    compile_role('v3-corrupt-verifier.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_NEW'])

    compile_role('v3-resize-owner.html', resize_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER'])
    compile_role('v3-resize-mutator.html', resize_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_RESIZER'])
    compile_role('v3-resize-verifier.html', resize_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_EXPECT_RESIZED_SHORT'])

    compile_role('v3-forced-error-owner.html', forced_error_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_OWNER'])
    compile_role('v3-forced-error-mutator.html', forced_error_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_COMMIT_ERROR'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V3_TEST_FORCED_COMMIT_ERROR=1'])
    compile_role('v3-forced-error-verifier.html', forced_error_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V3_TEST_VERIFIER'])

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kOwner = 0;
        const kMutator = 1;
        const kResizer = 2;
        const kVerifier = 3;
        const kCorruptor = 4;
        const kCommitError = 5;
        const kReady = 0;
        const kCorruptionRejected = 1;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v3-data';
        const kEventTimeoutMs = 20000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, predicate, description) {
          const index = module.events.findIndex(predicate);
          if (index >= 0) {
            return Promise.resolve(module.events.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const waiter = module.waiters.findIndex(
                (candidate) => candidate.resolve === resolve);
              if (waiter >= 0) {
                module.waiters.splice(waiter, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({predicate, resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          const waiter = module.waiters.findIndex(
            (candidate) => candidate.predicate(event.data));
          if (waiter >= 0) {
            const candidate = module.waiters.splice(waiter, 1)[0];
            clearTimeout(candidate.timeout);
            candidate.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runResult(path, role, result, description) {
          const module = launchModule(path);
          try {
            const message = await waitFor(
              module, (event) => event.event === 'result', description);
            if (message.role !== role || message.result !== result ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
          } finally {
            disposeModule(module);
          }
        }

        async function runAfterLeaseRelease(path, role, result, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(
                module, (event) => event.event === 'result', description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.result !== result ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function runInterruptedScenario(owner, mutator, verifier,
                                               phase, description) {
          await runResult(owner, kOwner, kReady, description + ' owner');
          const module = launchModule(mutator);
          try {
            const interruption = await waitFor(
              module,
              (event) => event.event === 'interrupt' && event.phase === phase,
              description + ' interruption');
            if (interruption.phase !== phase) {
              throw new Error(description + ' reported wrong interruption');
            }
          } finally {
            // This fresh-document disposal is the controlled interruption;
            // the parent never reads or writes OPFS directly.
            disposeModule(module);
          }
          await runAfterLeaseRelease(verifier, kVerifier, kReady,
                                     description + ' fresh verifier');
        }

        async function runBootstrapInterruption(owner, corruptor) {
          const module = launchModule(owner);
          try {
            await waitFor(
              module,
              (event) => event.event === 'interrupt' && event.phase === 0,
              'first bootstrap witness interruption');
          } finally {
            // A partial fixed-file set must be rejected rather than adopted or
            // recreated by the next fresh factory.
            disposeModule(module);
          }
          await runAfterLeaseRelease(corruptor, kCorruptor,
                                     kCorruptionRejected,
                                     'partial bootstrap factory rejection');
        }

        (async () => {
          await runBootstrapInterruption(
            'v3-bootstrap-owner.html', 'v3-bootstrap-corruptor.html');
          await runInterruptedScenario(
            'v3-before-owner.html', 'v3-before-mutator.html',
            'v3-before-verifier.html', 1,
            'old payload before CLEAN quorum and read-only recovery');
          await runInterruptedScenario(
            'v3-after-owner.html', 'v3-after-mutator.html',
            'v3-after-verifier.html', 2,
            'new payload after CLEAN quorum');
          await runResult('v3-corrupt-owner.html', kOwner, kReady,
                          'selected-chain owner');
          await runResult('v3-corrupt-mutator.html', kMutator, kReady,
                          'selected-chain update');
          for (const name of ['phase', 'descriptor', 'manifest']) {
            await runAfterLeaseRelease('v3-corrupt-' + name + '.html',
                                       kCorruptor, kCorruptionRejected,
                                       'selected ' + name +
                                       ' corruption rejection');
          }
          await runAfterLeaseRelease('v3-corrupt-verifier.html', kVerifier,
                                     kReady,
                                     'normal verifier after corruption tests');
          await runResult('v3-resize-owner.html', kOwner, kReady,
                          'resize owner');
          await runResult('v3-resize-mutator.html', kResizer, kReady,
                          'paired ftruncate grow and shrink');
          await runAfterLeaseRelease('v3-resize-verifier.html', kVerifier,
                                     kReady,
                                     'fresh exact short payload after resize');
          await runResult('v3-forced-error-owner.html', kOwner, kReady,
                          'forced-error owner');
          // This is an in-process pre-write fault injection, not a physical
          // storage-corruption or browser-crash test. It proves both the
          // failing attached pwrite and the fatal-latch check on a later open.
          await runResult('v3-forced-error-mutator.html', kCommitError,
                          kReady, 'forced commit error and reopen rejection');
          await runAfterLeaseRelease('v3-forced-error-verifier.html',
                                     kVerifier, kReady,
                                     'fresh payload after synthetic error');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_manifest(self):
    # V4 remains a non-mountable opaque-manifest control primitive. These
    # fresh-iframe tests use >128 KiB deterministic blobs, including embedded
    # test root/high-water fields, to prove normal persistence; old selection
    # before the mirrored phase witness; new selection after it; and a later
    # mutation after each recovered state. A real first-bootstrap interruption
    # and post-native-read selected-record fault variants fail factory creation
    # closed. Live phase/bootstrap validation faults also poison an open
    # primitive before a subsequent commit. They do not claim a filesystem,
    # directory, database, physical crash, power-loss, or Chromium-profile
    # recovery proof.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_manifest.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    normal_profile = f'wasmfs_profile_log_v4_normal_{random.getrandbits(64):016x}'
    before_profile = f'wasmfs_profile_log_v4_before_{random.getrandbits(64):016x}'
    after_profile = f'wasmfs_profile_log_v4_after_{random.getrandbits(64):016x}'
    bootstrap_profile = f'wasmfs_profile_log_v4_bootstrap_{random.getrandbits(64):016x}'
    corrupt_profile = f'wasmfs_profile_log_v4_corrupt_{random.getrandbits(64):016x}'
    live_profile = f'wasmfs_profile_log_v4_live_{random.getrandbits(64):016x}'

    def profile_arg(profile):
      return '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_PROFILE_NAME=' + profile

    def compile_role(output, profile, role_args, extra_args=None):
      self.compile_btest(
        test,
        common_args + [profile_arg(profile)] + role_args +
          (extra_args or []) + ['-o', output],
        reporting=Reporting.NONE)

    compile_role('v4-normal-owner.html', normal_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER'])
    compile_role('v4-normal-mutator.html', normal_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_MUTATOR'])
    compile_role('v4-normal-verifier.html', normal_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=1',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_POST_RECOVERY_MUTATION'])
    compile_role('v4-normal-post-verifier.html', normal_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=2'])

    compile_role('v4-before-owner.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER'])
    compile_role('v4-before-interruptor.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPTOR',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT_PHASE=1'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1'])
    compile_role('v4-before-recovery-verifier.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=0'])
    compile_role('v4-before-verifier.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=0',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_POST_RECOVERY_MUTATION'])
    compile_role('v4-before-post-verifier.html', before_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=2'])

    compile_role('v4-after-owner.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER'])
    compile_role('v4-after-interruptor.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPTOR',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT_PHASE=2'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1'])
    compile_role('v4-after-recovery-verifier.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=1'])
    compile_role('v4-after-verifier.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=1',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_POST_RECOVERY_MUTATION'])
    compile_role('v4-after-post-verifier.html', after_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=2'])

    compile_role('v4-bootstrap-owner.html', bootstrap_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT_PHASE=0'],
                 ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1'])
    compile_role('v4-bootstrap-corruptor.html', bootstrap_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_CORRUPTOR'])

    compile_role('v4-corrupt-owner.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER'])
    compile_role('v4-corrupt-mutator.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_MUTATOR'])
    for selector, name in ((1, 'phase'), (2, 'descriptor'),
                           (3, 'header'), (4, 'payload')):
      compile_role(f'v4-corrupt-{name}.html', corrupt_profile,
                   ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_CORRUPTOR'],
                   ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_SELECTED_CORRUPTION=' +
                    str(selector)])
    compile_role('v4-corrupt-verifier.html', corrupt_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=1'])

    compile_role('v4-live-owner.html', live_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_OWNER'])
    compile_role('v4-live-mutator.html', live_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_MUTATOR'])
    for selector, name in ((1, 'phase'), (2, 'bootstrap')):
      compile_role(f'v4-live-{name}.html', live_profile,
                   ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTOR'],
                   ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_LIVE_CORRUPTION=' +
                    str(selector)])
    compile_role('v4-live-verifier.html', live_profile,
                 ['-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_VERIFIER',
                  '-DWASMFS_OPFS_PROFILE_LOG_V4_TEST_EXPECTED_MANIFEST=1'])

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kOwner = 0;
        const kMutator = 1;
        const kVerifier = 2;
        const kInterruptor = 3;
        const kCorruptor = 4;
        const kLiveCorruptor = 5;
        const kReady = 0;
        const kCorruptionRejected = 1;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v4-manifest';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, predicate, description) {
          const index = module.events.findIndex(predicate);
          if (index >= 0) {
            return Promise.resolve(module.events.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const waiter = module.waiters.findIndex(
                (candidate) => candidate.resolve === resolve);
              if (waiter >= 0) {
                module.waiters.splice(waiter, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({predicate, resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          const waiter = module.waiters.findIndex(
            (candidate) => candidate.predicate(event.data));
          if (waiter >= 0) {
            const candidate = module.waiters.splice(waiter, 1)[0];
            clearTimeout(candidate.timeout);
            candidate.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runResult(path, role, description) {
          const module = launchModule(path);
          try {
            const message = await waitFor(
              module, (event) => event.event === 'result', description);
            if (message.role !== role || message.result !== kReady ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
          } finally {
            disposeModule(module);
          }
        }

        async function runAfterLeaseRelease(path, role, result, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(
                module, (event) => event.event === 'result', description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.result !== result ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function runVerifierAfterRelease(path, description) {
          await runAfterLeaseRelease(path, kVerifier, kReady, description);
        }

        async function runInterruptedScenario(owner, interruptor,
                                               recoveryVerifier, verifier,
                                               postVerifier, phase,
                                               description) {
          await runResult(owner, kOwner, description + ' owner');
          const module = launchModule(interruptor);
          try {
            const interruption = await waitFor(
              module,
              (event) => event.event === 'interrupt' && event.phase === phase,
              description + ' interruption');
            if (interruption.phase !== phase) {
              throw new Error(description + ' reported wrong witness');
            }
          } finally {
            // This controlled fresh-document disposal is the interruption;
            // the parent neither reads nor writes OPFS.
            disposeModule(module);
          }
          await runVerifierAfterRelease(
            recoveryVerifier,
            description + ' fresh selection before retry mutation');
          await runVerifierAfterRelease(verifier,
                                        description + ' recovery mutation');
          await runVerifierAfterRelease(postVerifier,
                                        description + ' fresh post mutation');
        }

        async function runBootstrapInterruption(owner, corruptor) {
          const module = launchModule(owner);
          try {
            const interruption = await waitFor(
              module,
              (event) => event.event === 'interrupt' && event.phase === 0,
              'partial bootstrap interruption');
            if (interruption.phase !== 0) {
              throw new Error('partial bootstrap reported wrong witness');
            }
          } finally {
            // The only partial physical state comes from the V4 factory;
            // this page neither reads nor writes OPFS.
            disposeModule(module);
          }
          await runAfterLeaseRelease(
            corruptor, kCorruptor, kCorruptionRejected,
            'partial bootstrap factory rejection');
        }

        (async () => {
          await runResult('v4-normal-owner.html', kOwner, 'normal owner');
          await runResult('v4-normal-mutator.html', kMutator,
                          'normal 192 KiB manifest update');
          await runVerifierAfterRelease('v4-normal-verifier.html',
                                        'normal fresh verifier and mutation');
          await runVerifierAfterRelease('v4-normal-post-verifier.html',
                                        'normal fresh post mutation');
          await runInterruptedScenario(
            'v4-before-owner.html', 'v4-before-interruptor.html',
            'v4-before-recovery-verifier.html', 'v4-before-verifier.html',
            'v4-before-post-verifier.html', 1,
            'old manifest before mirrored witness');
          await runInterruptedScenario(
            'v4-after-owner.html', 'v4-after-interruptor.html',
            'v4-after-recovery-verifier.html', 'v4-after-verifier.html',
            'v4-after-post-verifier.html', 2,
            'new manifest after mirrored witness');
          await runBootstrapInterruption(
            'v4-bootstrap-owner.html', 'v4-bootstrap-corruptor.html');
          await runResult('v4-corrupt-owner.html', kOwner,
                          'selected-record owner');
          await runResult('v4-corrupt-mutator.html', kMutator,
                          'selected-record update');
          for (const name of ['phase', 'descriptor', 'header', 'payload']) {
            await runAfterLeaseRelease(
              'v4-corrupt-' + name + '.html', kCorruptor,
              kCorruptionRejected,
              'selected ' + name + ' corruption rejection');
          }
          await runVerifierAfterRelease(
            'v4-corrupt-verifier.html',
            'normal verifier after selected corruption tests');
          await runResult('v4-live-owner.html', kOwner,
                          'live-record owner');
          await runResult('v4-live-mutator.html', kMutator,
                          'live-record update');
          for (const name of ['phase', 'bootstrap']) {
            await runAfterLeaseRelease(
              'v4-live-' + name + '.html', kLiveCorruptor,
              kCorruptionRejected,
              'live ' + name + ' corruption rejection and poison');
          }
          await runVerifierAfterRelease(
            'v4-live-verifier.html',
            'normal verifier after live corruption tests');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=240)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem(self):
    # The V4 filesystem is an isolated WasmFS mount proof. Fresh documents
    # cover its durable inode tree, copy-on-write data, metadata, namespace,
    # directory flush, and orderly lease handoff. This does not yet claim
    # database recovery, browser-crash/power-loss recovery, record-lock
    # success, or Chromium-profile activation.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = (
      f'wasmfs_profile_log_v4_filesystem_{random.getrandbits(64):016x}')
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_PROFILE_NAME=' + profile)

    def compile_role(output, role):
      self.compile_btest(
        test, common_args + [profile_arg, role, '-o', output],
        reporting=Reporting.NONE)

    compile_role('v4fs-owner.html',
                 '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_OWNER')
    compile_role('v4fs-mutator.html',
                 '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_MUTATOR')
    compile_role('v4fs-verifier.html',
                 '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TEST_VERIFIER')

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kOwner = 0;
        const kMutator = 1;
        const kVerifier = 2;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v4-filesystem';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease('v4fs-owner.html', kOwner,
                                     'V4 filesystem owner');
          await runAfterLeaseRelease('v4fs-mutator.html', kMutator,
                                     'fresh V4 filesystem mutator');
          await runAfterLeaseRelease('v4fs-verifier.html', kVerifier,
                                     'fresh V4 filesystem verifier');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_format_namespace(self):
    # The opaque V4 manifest and mountable V4 filesystem use one logical
    # profile name in four fresh documents. A valid opaque manifest must not
    # be parsed as filesystem state, filesystem writes must not overwrite it,
    # and a later opaque mutation must not overwrite the filesystem sentinel.
    # This is physical-format namespace isolation only, not a concurrent-
    # writer, database, browser-crash, or Chromium-profile test.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_format_namespace.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = (
      f'wasmfs_profile_log_v4_format_namespace_{random.getrandbits(64):016x}')
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_PROFILE_NAME=' +
      profile)

    def compile_role(output, role):
      self.compile_btest(
        test, common_args + [profile_arg, role, '-o', output],
        reporting=Reporting.NONE)

    compile_role(
      'v4-format-manifest-owner.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_OWNER')
    compile_role(
      'v4-format-filesystem-owner.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_OWNER')
    compile_role(
      'v4-format-manifest-mutator.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_MANIFEST_MUTATOR')
    compile_role(
      'v4-format-filesystem-verifier.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FORMAT_NAMESPACE_TEST_FILESYSTEM_VERIFIER')

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kManifestOwner = 0;
        const kFilesystemOwner = 1;
        const kManifestMutator = 2;
        const kFilesystemVerifier = 3;
        const kReady = 0;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v4-format-namespace';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, predicate, description) {
          const index = module.events.findIndex(predicate);
          if (index >= 0) {
            return Promise.resolve(module.events.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const waiter = module.waiters.findIndex(
                (candidate) => candidate.resolve === resolve);
              if (waiter >= 0) {
                module.waiters.splice(waiter, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({predicate, resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          const waiter = module.waiters.findIndex(
            (candidate) => candidate.predicate(event.data));
          if (waiter >= 0) {
            const candidate = module.waiters.splice(waiter, 1)[0];
            clearTimeout(candidate.timeout);
            candidate.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(
                module, (event) => event.event === 'result', description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.result !== kReady ||
                message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', result=' + message.result + ', errno=' +
                              message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease(
            'v4-format-manifest-owner.html', kManifestOwner,
            'V4 opaque-manifest owner');
          await runAfterLeaseRelease(
            'v4-format-filesystem-owner.html', kFilesystemOwner,
            'fresh V4 filesystem owner with the same profile');
          await runAfterLeaseRelease(
            'v4-format-manifest-mutator.html', kManifestMutator,
            'fresh V4 opaque-manifest mutator');
          await runAfterLeaseRelease(
            'v4-format-filesystem-verifier.html', kFilesystemVerifier,
            'fresh V4 filesystem verifier');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_fail_closed_retirement(self):
    # A higher-level profile owner can reject its own close/fence result after
    # all WasmFS writes succeeded. The explicit failure-retirement ABI must
    # close V4's private physical OPFS files without releasing the Web Lock,
    # survive the holder's actual EXIT_RUNTIME destruction, then allow a fresh
    # document to recover the already-synced marker and drain normally.
    test = 'wasmfs/wasmfs_opfs_profile_fail_closed_retirement.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = ('wasmfs_profile_fail_closed_retirement_%016x' %
               random.getrandbits(64))
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_PROFILE_NAME=' +
      profile)

    create_file('profile-fail-closed-holder-pre.js', r'''
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-profile-fail-closed-retirement',
          },
          window.location.origin);
      };
      Module['onAbort'] = (reason) => {
        window.parent.postMessage(
          {
            event: 'holder-abort',
            reason: String(reason),
            type: 'wasmfs-opfs-profile-fail-closed-retirement',
          },
          window.location.origin);
      };
    ''')

    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_HOLDER',
        '-sEXIT_RUNTIME',
        '--pre-js',
        'profile-fail-closed-holder-pre.js',
        '-o',
        'profile-fail-closed-holder.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_CONTENDER',
        '-o',
        'profile-fail-closed-contender.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_VERIFIER',
        '-o',
        'profile-fail-closed-verifier.html',
      ],
      reporting=Reporting.NONE)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kVerifier = 2;
        const kReady = 0;
        const kBusy = 1;
        const kWitnessType = 'wasmfs-opfs-profile-fail-closed-retirement';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kEventTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, reject, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder exit'));
            }, kEventTimeoutMs);
            pendingHolderExits.set(frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          if (event.data.event === 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status !== 0) {
              pending.reject(new Error('holder exited with status ' +
                                       event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          const pendingExit = pendingHolderExits.get(event.source);
          if (event.data.event === 'holder-abort' && pendingExit) {
            pendingHolderExits.delete(event.source);
            clearTimeout(pendingExit.timeout);
            pendingExit.reject(new Error('holder aborted: ' +
                                         event.data.reason));
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          if (event.data.event === 'holder-abort') {
            pending.frame.remove();
            pending.reject(new Error('holder aborted: ' + event.data.reason));
            return;
          }
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function expectBusyContender() {
          const contender = await startModule('profile-fail-closed-contender.html');
          try {
            if (contender.message.role !== kContender ||
                contender.message.result !== kBusy ||
                contender.message.error === 0) {
              throw new Error('contender acquired a retained failure lease');
            }
          } finally {
            contender.frame.remove();
          }
        }

        async function runVerifierAfterHolderDestruction() {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const verifier = await startModule('profile-fail-closed-verifier.html');
            try {
              if (verifier.message.role !== kVerifier) {
                throw new Error('verifier reported role ' + verifier.message.role);
              }
              if (verifier.message.result === kBusy) {
                await delay(100);
                continue;
              }
              if (verifier.message.result !== kReady ||
                  verifier.message.error !== 0) {
                throw new Error('verifier failed: result=' +
                                verifier.message.result + ', errno=' +
                                verifier.message.error);
              }
              return;
            } finally {
              verifier.frame.remove();
            }
          }
          throw new Error('retained failure lease never left destroyed holder');
        }

        (async () => {
          const holder = await startModule('profile-fail-closed-holder.html');
          if (holder.message.role !== kHolder ||
              holder.message.result !== kReady || holder.message.error !== 0) {
            holder.frame.remove();
            throw new Error('holder failure retirement failed: result=' +
                            holder.message.result + ', errno=' +
                            holder.message.error);
          }
          await expectBusyContender();
          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_profile_fail_closed_retirement_holder_shutdown();
          await holderExit;
          holder.frame.remove();
          await runVerifierAfterHolderDestruction();
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_proxy_completion_failure(self):
    # This controlled V4 acknowledgement-loss witness starts from a separately
    # drained A, faults B exactly once only after its replacement manifest has
    # really flushed but before outer V4 publication, then proves the failed
    # holder retains its lease. After actual holder EXIT_RUNTIME, a fresh
    # document must read A rather than B, publish C, and a further fresh
    # reload must read C. This is not a literal ProxyWorker failure, browser
    # crash, power loss, or OPFS-directory-durability simulation.
    test = 'wasmfs/wasmfs_opfs_profile_fail_closed_retirement.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = ('wasmfs_profile_log_v4_proxy_completion_%016x' %
               random.getrandbits(64))
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_FAIL_CLOSED_RETIREMENT_TEST_PROFILE_NAME=' +
      profile)

    create_file('profile-v4-proxy-completion-holder-pre.js', r'''
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-profile-log-v4-proxy-completion',
          },
          window.location.origin);
      };
      Module['onAbort'] = (reason) => {
        window.parent.postMessage(
          {
            event: 'holder-abort',
            reason: String(reason),
            type: 'wasmfs-opfs-profile-log-v4-proxy-completion',
          },
          window.location.origin);
      };
    ''')

    def compile_role(output, role, extra_args=None):
      args = common_args + [profile_arg, role]
      if extra_args:
        args += extra_args
      self.compile_btest(
        test, args + ['-o', output], reporting=Reporting.NONE)

    compile_role(
      'v4-proxy-completion-seed.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_SEED')
    compile_role(
      'v4-proxy-completion-holder.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_HOLDER',
      [
        '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_PROXY_COMPLETION_FAILURE=1',
        # PROXY_TO_PTHREAD, the mounted V4 ProxyWorker, and the affinity
        # probe's child each need a worker. Preallocate all three so the
        # barrier protocol cannot rely on dynamic worker creation.
        '-sPTHREAD_POOL_SIZE=3',
        '-sPTHREAD_POOL_SIZE_STRICT=2',
        '-sEXIT_RUNTIME',
        '--pre-js',
        'profile-v4-proxy-completion-holder-pre.js',
      ])
    compile_role(
      'v4-proxy-completion-contender.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_CONTENDER')
    compile_role(
      'v4-proxy-completion-verifier.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_VERIFIER')
    compile_role(
      'v4-proxy-completion-reload.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_PROXY_COMPLETION_TEST_RELOAD')

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 3;
        const kHolder = 4;
        const kContender = 5;
        const kVerifier = 6;
        const kReload = 7;
        const kReady = 0;
        const kBusy = 1;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-proxy-completion';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kEventTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, reject, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder exit'));
            }, kEventTimeoutMs);
            pendingHolderExits.set(frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          if (event.data.event === 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status !== 0) {
              pending.reject(new Error('holder exited with status ' +
                                       event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          const pendingExit = pendingHolderExits.get(event.source);
          if (event.data.event === 'holder-abort' && pendingExit) {
            pendingHolderExits.delete(event.source);
            clearTimeout(pendingExit.timeout);
            pendingExit.reject(new Error('holder aborted: ' +
                                         event.data.reason));
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          if (event.data.event === 'holder-abort') {
            pending.frame.remove();
            pending.reject(new Error('holder aborted: ' + event.data.reason));
            return;
          }
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = await startModule(path);
            try {
              if (module.message.role !== role) {
                throw new Error(description + ' reported role ' +
                                module.message.role);
              }
              // The native role reports kBusy only after it compared errno
              // against EBUSY. Keep that ABI assertion in C rather than
              // duplicating a host-side errno number here.
              if (module.message.result === kBusy &&
                  module.message.error !== 0) {
                await delay(100);
                continue;
              }
              if (module.message.result !== kReady ||
                  module.message.error !== 0) {
                throw new Error(description + ' failed: result=' +
                                module.message.result + ', errno=' +
                                module.message.error);
              }
              return;
            } finally {
              module.frame.remove();
            }
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function expectBusyContender() {
          const contender = await startModule('v4-proxy-completion-contender.html');
          try {
            if (contender.message.role !== kContender ||
                contender.message.result !== kBusy ||
                contender.message.error === 0) {
              throw new Error('contender did not observe retained EBUSY lease: '
                              + 'role=' + contender.message.role +
                              ', result=' + contender.message.result +
                              ', errno=' + contender.message.error);
            }
          } finally {
            contender.frame.remove();
          }
        }

        (async () => {
          await runAfterLeaseRelease(
            'v4-proxy-completion-seed.html', kSeed,
            'durable A seed');
          const holder =
            await startModule('v4-proxy-completion-holder.html');
          if (holder.message.role !== kHolder ||
              holder.message.result !== kReady || holder.message.error !== 0) {
            holder.frame.remove();
            throw new Error('post-flush B holder failed: result=' +
                            holder.message.result + ', errno=' +
                            holder.message.error);
          }
          await expectBusyContender();
          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_profile_fail_closed_retirement_holder_shutdown();
          await holderExit;
          holder.frame.remove();
          await runAfterLeaseRelease(
            'v4-proxy-completion-verifier.html', kVerifier,
            'fresh A recovery and C publication');
          await runAfterLeaseRelease(
            'v4-proxy-completion-reload.html', kReload,
            'fresh C reload');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_bootstrap_recovery(self):
    # Every safe native bootstrap cut point runs in a fresh iframe. The parent
    # discards that document at the exact durable witness checkpoint and proves
    # that a new document either mounts, writes, reads, and drains the profile,
    # or rejects an intentionally incomplete bootstrap without creating a
    # replacement root. Separate runs interrupt cleanup after each fixed-name
    # deletion, so bootstrap-last retirement is exercised rather than assumed.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem_recovery.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']

    def profile_arg(profile):
      return ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_PROFILE_NAME='
              + profile)

    def compile_role(output, profile, role, phase=None, extra_args=None):
      args = common_args + [profile_arg(profile), role]
      if phase is not None:
        args += [
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPT_PHASE='
          + str(phase),
          '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1',
        ]
      if extra_args:
        args += extra_args
      self.compile_btest(test, args + ['-o', output], reporting=Reporting.NONE)

    recovery_cases = []
    for phase in (0, 1, 2, 3, 4):
      profile = ('wasmfs_profile_log_v4_bootstrap_recovery_%d_%016x' %
                 (phase, random.getrandbits(64)))
      interruptor = 'v4fs-bootstrap-%d-interruptor.html' % phase
      verifier = 'v4fs-bootstrap-%d-verifier.html' % phase
      reload = 'v4fs-bootstrap-%d-reload.html' % phase
      compile_role(
        interruptor, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR',
        phase)
      compile_role(
        verifier, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_VERIFIER')
      compile_role(
        reload, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_RELOAD')
      recovery_cases.append((phase, interruptor, verifier, reload))

    fail_closed_cases = []
    for phase in (-2, -1):
      profile = ('wasmfs_profile_log_v4_bootstrap_fail_closed_%d_%016x' %
                 (-phase, random.getrandbits(64)))
      interruptor = 'v4fs-bootstrap-fail-%d-interruptor.html' % -phase
      verifier = 'v4fs-bootstrap-fail-%d-verifier.html' % -phase
      compile_role(
        interruptor, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR',
        phase)
      compile_role(
        verifier, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_FAIL_CLOSED')
      fail_closed_cases.append((phase, interruptor, verifier))

    empty_post_root_profile = (
      'wasmfs_profile_log_v4_empty_post_root_%016x' %
      random.getrandbits(64))
    compile_role(
      'v4fs-bootstrap-empty-post-root-interruptor.html',
      empty_post_root_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR',
      9,
      ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_EMPTY_POST_ROOT_MANIFEST=1'])
    compile_role(
      'v4fs-bootstrap-empty-post-root-verifier.html',
      empty_post_root_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_FAIL_CLOSED')

    cleanup_cases = []
    for phase in (5, 6, 7, 8):
      profile = ('wasmfs_profile_log_v4_bootstrap_cleanup_%d_%016x' %
                 (phase, random.getrandbits(64)))
      seed = 'v4fs-bootstrap-cleanup-%d-seed.html' % phase
      interruptor = 'v4fs-bootstrap-cleanup-%d-interruptor.html' % phase
      verifier = 'v4fs-bootstrap-cleanup-%d-verifier.html' % phase
      reload = 'v4fs-bootstrap-cleanup-%d-reload.html' % phase
      # Phase 1 has a mirrored PREPARED pair and all three sibling names, but
      # no mountable root. It is the seed state for each delete interruption.
      compile_role(
        seed, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR',
        1)
      compile_role(
        interruptor, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_INTERRUPTOR',
        phase)
      compile_role(
        verifier, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_VERIFIER')
      compile_role(
        reload, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_RECOVERY_TEST_RELOAD')
      cleanup_cases.append((phase, seed, interruptor, verifier, reload))

    recovery_script = '\n'.join(
      "          await interruptAfterRelease('%s', %d, 'bootstrap phase %d');\n"
      "          await runAfterLeaseRelease('%s', kVerifier, "
      "'bootstrap phase %d recovery verifier');\n"
      "          await runAfterLeaseRelease('%s', kReload, "
      "'bootstrap phase %d fresh reload');" %
      (interruptor, phase, phase, verifier, phase, reload, phase)
      for phase, interruptor, verifier, reload in recovery_cases)
    fail_closed_script = '\n'.join(
      "          await interruptAfterRelease('%s', %d, 'incomplete bootstrap phase %d');\n"
      "          await runAfterLeaseRelease('%s', kFailClosed, "
      "'incomplete bootstrap phase %d fail-closed verifier');" %
      (interruptor, phase, phase, verifier, phase)
      for phase, interruptor, verifier in fail_closed_cases)
    cleanup_script = '\n'.join(
      "          await interruptAfterRelease('%s', 1, 'cleanup phase %d seed');\n"
      "          await interruptAfterRelease('%s', %d, 'cleanup phase %d interruption');\n"
      "          await runAfterLeaseRelease('%s', kVerifier, "
      "'cleanup phase %d recovery verifier');\n"
      "          await runAfterLeaseRelease('%s', kReload, "
      "'cleanup phase %d fresh reload');" %
      (seed, phase, interruptor, phase, phase, verifier, phase, reload, phase)
      for phase, seed, interruptor, verifier, reload in cleanup_cases)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kInterruptor = 0;
        const kVerifier = 1;
        const kReload = 2;
        const kFailClosed = 3;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v4-filesystem-recovery';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function interruptAfterRelease(path, checkpoint, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.event !== 'interrupt' ||
                message.checkpoint !== checkpoint) {
              throw new Error(description + ' did not reach checkpoint ' +
                              checkpoint + ': role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
%(recovery_script)s
%(fail_closed_script)s
          await interruptAfterRelease(
            'v4fs-bootstrap-empty-post-root-interruptor.html', 9,
            'empty post-root manifest interruption');
          await runAfterLeaseRelease(
            'v4fs-bootstrap-empty-post-root-verifier.html', kFailClosed,
            'empty post-root manifest fail-closed verifier');
%(cleanup_script)s
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''' % {
      'recovery_script': recovery_script,
      'fail_closed_script': fail_closed_script,
      'cleanup_script': cleanup_script,
    })
    self.run_browser('a.html', '/report_result?0', timeout=240)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_mutation_recovery(self):
    # Interrupt real mounted-filesystem transactions after the durable
    # descriptor pair or at the V4 phase quorum, then reopen in fresh
    # documents. The descriptor-pair boundary and phase one must select the
    # complete old data/tree; phase two must select the complete new data/tree.
    # Every recovery result commits another directory mutation and reloads it.
    # This is controlled iframe-disposal evidence, not a physical-crash,
    # database, or Chromium-profile persistence claim.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem_mutation_recovery.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']

    def profile_arg(profile):
      return ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_'
              'PROFILE_NAME=' + profile)

    def compile_role(output, profile, mutation, role, phase=None,
                     expect_new=None):
      args = common_args + [profile_arg(profile), mutation, role]
      if phase is not None:
        args += [
          ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_'
           'INTERRUPT_PHASE=' + str(phase)),
          '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1',
        ]
      if expect_new is not None:
        args += [
          ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_'
           'EXPECT_NEW=' + str(int(expect_new))),
        ]
      self.compile_btest(test, args + ['-o', output], reporting=Reporting.NONE)

    cases = []
    for mutation_name, mutation in (
        ('data', '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_DATA'),
        ('rename', '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RENAME')):
      for phase in (10, 1, 2):
        profile = ('wasmfs_profile_log_v4_filesystem_%s_recovery_%d_%016x' %
                   (mutation_name, phase, random.getrandbits(64)))
        prefix = 'v4fs-%s-recovery-%d' % (mutation_name, phase)
        seed = prefix + '-seed.html'
        interruptor = prefix + '-interruptor.html'
        verifier = prefix + '-verifier.html'
        reload = prefix + '-reload.html'
        compile_role(
          seed, profile, mutation,
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_SEED')
        compile_role(
          interruptor, profile, mutation,
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_INTERRUPTOR',
          phase=phase)
        compile_role(
          verifier, profile, mutation,
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_VERIFIER',
          expect_new=phase == 2)
        compile_role(
          reload, profile, mutation,
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_MUTATION_RECOVERY_TEST_RELOAD',
          expect_new=phase == 2)
        cases.append((mutation_name, phase, seed, interruptor, verifier, reload))

    case_script = '\n'.join(
      "          await runAfterLeaseRelease('%s', kSeed, '%s phase %d seed');\n"
      "          await interruptAfterLeaseRelease('%s', %d, '%s phase %d interruption');\n"
      "          await runAfterLeaseRelease('%s', kVerifier, '%s phase %d recovery verifier');\n"
      "          await runAfterLeaseRelease('%s', kReload, '%s phase %d fresh reload');" %
      (seed, mutation_name, phase, interruptor, phase, mutation_name, phase,
       verifier, mutation_name, phase, reload, mutation_name, phase)
      for mutation_name, phase, seed, interruptor, verifier, reload in cases)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 0;
        const kInterruptor = 1;
        const kVerifier = 2;
        const kReload = 3;
        const kBusy = 16;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-filesystem-mutation-recovery';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function interruptAfterLeaseRelease(path, checkpoint, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.event !== 'interrupt' ||
                message.checkpoint !== checkpoint) {
              throw new Error(description + ' did not reach checkpoint ' +
                              checkpoint + ': role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
%(case_script)s
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''' % {'case_script': case_script})
    self.run_browser('a.html', '/report_result?0', timeout=240)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_sqlite_recovery(self):
    # SQLite uses an observed DELETE rollback journal with synchronous=FULL on
    # the mounted V4 filesystem.  An un-interrupted fresh-document control
    # commits and reloads B.  Each interruption is armed only while COMMIT B
    # is executing and stops after a V4 durable descriptor-pair, phase-one, or
    # phase-two publication.  A fresh document must pass integrity_check and
    # recover one complete A/B pair, never a mixed pair, before it commits and
    # freshly reopens C.  This is controlled iframe-disposal evidence, not a
    # physical power-loss, WAL, LevelDB, or Chromium-profile persistence claim.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem_sqlite_recovery.c'
    common_args = [
      '-sWASMFS',
      '-pthread',
      '-sPROXY_TO_PTHREAD',
      '-lopfs.js',
      '-sUSE_SQLITE3',
    ]

    def profile_arg(profile):
      return ('-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_PROFILE_NAME=' +
              profile)

    def compile_role(output, profile, role, phase=None, reload_state=None):
      args = common_args + [profile_arg(profile), role]
      if phase is not None:
        args += [
          ('-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPT_PHASE=' +
           str(phase)),
          '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1',
        ]
      if reload_state is not None:
        args += [
          ('-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD_STATE=' +
           str(reload_state)),
        ]
      self.compile_btest(test, args + ['-o', output], reporting=Reporting.NONE)

    control_profile = ('wasmfs_profile_log_v4_filesystem_sqlite_recovery_control_%016x' %
                       random.getrandbits(64))
    control_seed = 'v4fs-sqlite-recovery-control-seed.html'
    control = 'v4fs-sqlite-recovery-control.html'
    control_reload = 'v4fs-sqlite-recovery-control-reload.html'
    compile_role(
      control_seed, control_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_SEED')
    compile_role(
      control, control_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_CONTROL')
    compile_role(
      control_reload, control_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD',
      reload_state=2)

    cases = []
    for phase in (10, 1, 2):
      profile = ('wasmfs_profile_log_v4_filesystem_sqlite_recovery_%d_%016x' %
                 (phase, random.getrandbits(64)))
      prefix = 'v4fs-sqlite-recovery-%d' % phase
      seed = prefix + '-seed.html'
      interruptor = prefix + '-interruptor.html'
      verifier = prefix + '-verifier.html'
      reload = prefix + '-reload.html'
      compile_role(
        seed, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_SEED')
      compile_role(
        interruptor, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_INTERRUPTOR',
        phase=phase)
      compile_role(
        verifier, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_VERIFIER')
      compile_role(
        reload, profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_SQLITE_RECOVERY_TEST_RELOAD')
      cases.append((phase, seed, interruptor, verifier, reload))

    case_script = '\n'.join(
      "          await runAfterLeaseRelease('%s', kSeed, 'SQLite phase %d seed');\n"
      "          await interruptAfterLeaseRelease('%s', %d, 'SQLite phase %d interruption');\n"
      "          await runAfterLeaseRelease('%s', kVerifier, 'SQLite phase %d recovery verifier');\n"
      "          await runAfterLeaseRelease('%s', kReload, 'SQLite phase %d fresh reload');" %
      (seed, phase, interruptor, phase, phase, verifier, phase, reload, phase)
      for phase, seed, interruptor, verifier, reload in cases)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 0;
        const kInterruptor = 1;
        const kVerifier = 2;
        const kReload = 3;
        const kControl = 4;
        const kBusy = 16;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-filesystem-sqlite-recovery';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function interruptAfterLeaseRelease(path, checkpoint, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.event !== 'interrupt' ||
                message.checkpoint !== checkpoint ||
                message.role !== kInterruptor || message.error !== 0) {
              throw new Error(description + ' did not reach checkpoint ' +
                              checkpoint + ': role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease('%(control_seed)s', kSeed,
                                     'SQLite un-interrupted control seed');
          await runAfterLeaseRelease('%(control)s', kControl,
                                     'SQLite un-interrupted control commit');
          await runAfterLeaseRelease('%(control_reload)s', kReload,
                                     'SQLite un-interrupted control reload');
%(case_script)s
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''' % {
      'case_script': case_script,
      'control_seed': control_seed,
      'control': control,
      'control_reload': control_reload,
    })
    self.run_browser('a.html', '/report_result?0', timeout=300)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_tail_recovery(self):
    # A phase-one interruption leaves a real unreachable V4 append tail. The
    # parent only reads the named test artifacts to establish that boundary;
    # fresh WasmFS documents perform all recovery and mutation. The injected
    # truncate failure is not physical quota, power loss, database, or Chrome
    # profile evidence.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem_tail_recovery.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = ('wasmfs_profile_log_v4_filesystem_tail_recovery_%016x' %
               random.getrandbits(64))
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_PROFILE_NAME=' +
      profile)

    def compile_role(output, role, extra_args=None):
      self.compile_btest(
        test, common_args + [profile_arg, role] + (extra_args or []) +
          ['-o', output], reporting=Reporting.NONE)

    compile_role(
      'v4fs-tail-seed.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_SEED')
    compile_role(
      'v4fs-tail-interruptor.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_INTERRUPTOR',
      [
        ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_'
         'INTERRUPT_PHASE=1'),
        '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1',
      ])
    compile_role(
      'v4fs-tail-trim-failure.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_TRIM_FAILURE',
      [
        '-sWASMFS_OPFS_TEST_QUOTA_TRUNCATE=1',
      ])
    compile_role(
      'v4fs-tail-verifier.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_VERIFIER')
    compile_role(
      'v4fs-tail-reload.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_TAIL_RECOVERY_TEST_RELOAD')

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 0;
        const kInterruptor = 1;
        const kTrimFailure = 2;
        const kVerifier = 3;
        const kReload = 4;
        const kBusy = 16;
        const kProfile = '%(profile)s';
        const kRecordSize = 128;
        const kControlSize = 6 * kRecordSize;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-filesystem-tail-recovery';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function require(condition, description) {
          if (!condition) {
            throw new Error(description);
          }
        }

        function requireMagic(bytes, offset, magic, description) {
          require(offset + magic.length <= bytes.byteLength,
                  description + ' is truncated');
          for (let index = 0; index < magic.length; ++index) {
            require(bytes[offset + index] === magic.charCodeAt(index),
                    description + ' has the wrong magic');
          }
        }

        function readU64(view, offset, description) {
          const value = view.getBigUint64(offset, true);
          require(value <= BigInt(Number.MAX_SAFE_INTEGER),
                  description + ' is not a safe integer');
          return Number(value);
        }

        function parsePhase(bytes, view, offset, description) {
          requireMagic(bytes, offset, 'WFSLG4P0', description);
          require(view.getUint32(offset + 8, true) === 4,
                  description + ' has the wrong version');
          require(view.getUint32(offset + 12, true) === kRecordSize,
                  description + ' has the wrong size');
          const generation = readU64(view, offset + 16, description);
          const arena = view.getUint32(offset + 24, true);
          require(generation !== 0 && arena < 2 && arena === (generation & 1),
                  description + ' is not a valid phase');
          require(view.getUint32(offset + 28, true) === 1,
                  description + ' is not clean');
          return {generation, arena};
        }

        function parseDescriptor(bytes, view, generation, arena, copy) {
          const offset = (((generation & 1) * 2 + copy) * kRecordSize);
          const description = 'descriptor ' + generation + '/' + copy;
          requireMagic(bytes, offset, 'WFSLG4D0', description);
          require(view.getUint32(offset + 8, true) === 4,
                  description + ' has the wrong version');
          require(view.getUint32(offset + 12, true) === kRecordSize,
                  description + ' has the wrong size');
          require(readU64(view, offset + 16, description) === generation &&
                  view.getUint32(offset + 24, true) === arena,
                  description + ' does not name the selected phase');
          return {
            highWater: [
              readU64(view, offset + 56, description),
              readU64(view, offset + 64, description),
            ],
            manifestOffset: readU64(view, offset + 32, description),
            manifestSize: readU64(view, offset + 40, description),
          };
        }

        function parseSelectedDescriptor(control) {
          require(control.byteLength === kControlSize,
                  'V4 control file has the wrong fixed size');
          const view = new DataView(control.buffer, control.byteOffset,
                                    control.byteLength);
          const first = parsePhase(control, view, 4 * kRecordSize, 'phase 0');
          const second = parsePhase(control, view, 5 * kRecordSize, 'phase 1');
          let selected = first;
          if (first.generation === second.generation) {
            require(first.arena === second.arena,
                    'mirrored V4 phases disagree');
          } else {
            const older = first.generation < second.generation ? first : second;
            const newer = first.generation < second.generation ? second : first;
            require(newer.generation === older.generation + 1,
                    'V4 phases do not form a recovery split');
            selected = older;
          }
          const descriptor0 = parseDescriptor(
            control, view, selected.generation, selected.arena, 0);
          const descriptor1 = parseDescriptor(
            control, view, selected.generation, selected.arena, 1);
          require(descriptor0.manifestOffset === descriptor1.manifestOffset &&
                  descriptor0.manifestSize === descriptor1.manifestSize &&
                  descriptor0.highWater[0] === descriptor1.highWater[0] &&
                  descriptor0.highWater[1] === descriptor1.highWater[1],
                  'mirrored V4 descriptors disagree');
          return {
            generation: selected.generation,
            highWater: descriptor0.highWater,
          };
        }

        async function readPhysicalLayout() {
          // This observer never mutates OPFS. Native V4 code selects and
          // validates the records; it only witnesses the physical tail after
          // the interrupted iframe has been disposed.
          const root = await navigator.storage.getDirectory();
          const stem = '.wasmfs-profile-log-v4-fs-' + kProfile.length + '-' +
                       kProfile;
          async function readFile(name) {
            const handle = await root.getFileHandle(name);
            return new Uint8Array(await (await handle.getFile()).arrayBuffer());
          }
          const control = await readFile(stem + '-control');
          const selected = parseSelectedDescriptor(control);
          const physical = await Promise.all([0, 1].map(async (arena) => {
            const handle = await root.getFileHandle(stem + '-arena-' + arena);
            return (await handle.getFile()).size;
          }));
          return {...selected, physical};
        }

        async function waitForPhysicalLayout(description) {
          let lastError;
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            try {
              return await readPhysicalLayout();
            } catch (error) {
              lastError = error;
              await delay(100);
            }
          }
          throw new Error(description + ' could not read test artifacts: ' +
                          lastError);
        }

        function requireTail(layout, description) {
          require(layout.physical.some(
                    (size, arena) => size > layout.highWater[arena]),
                  description + ' has no unreachable V4 arena tail');
        }

        function requireSameLayout(before, after, description) {
          require(before.generation === after.generation &&
                  before.highWater[0] === after.highWater[0] &&
                  before.highWater[1] === after.highWater[1] &&
                  before.physical[0] === after.physical[0] &&
                  before.physical[1] === after.physical[1],
                  description + ' changed the selected state or physical tail');
        }

        function requireExactPhysicalBounds(layout, description) {
          require(layout.physical[0] === layout.highWater[0] &&
                  layout.physical[1] === layout.highWater[1],
                  description + ' did not trim V4 arena tails to high water');
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', stage=' + message.stage +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function interruptAfterLeaseRelease(path, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.event !== 'interrupt' || message.checkpoint !== 1) {
              throw new Error(description + ' did not reach phase one: role=' +
                              message.role + ', stage=' + message.stage +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease('v4fs-tail-seed.html', kSeed,
                                     'V4 tail seed');
          await interruptAfterLeaseRelease('v4fs-tail-interruptor.html',
                                           'V4 tail interruption');
          const interrupted = await waitForPhysicalLayout(
            'V4 phase-one interrupted layout');
          requireTail(interrupted, 'V4 phase-one interruption');
          await runAfterLeaseRelease('v4fs-tail-trim-failure.html',
                                     kTrimFailure,
                                     'V4 injected tail-trim failure');
          const afterFailure = await waitForPhysicalLayout(
            'V4 quota-rejected tail-trim layout');
          requireSameLayout(interrupted, afterFailure,
                            'V4 injected tail-trim failure');
          requireTail(afterFailure, 'V4 quota-rejected tail trim');
          await runAfterLeaseRelease('v4fs-tail-verifier.html', kVerifier,
                                     'V4 normal tail-trim verifier');
          const repaired = await waitForPhysicalLayout(
            'V4 repaired tail layout');
          requireExactPhysicalBounds(repaired, 'V4 normal tail-trim verifier');
          await runAfterLeaseRelease('v4fs-tail-reload.html', kReload,
                                     'V4 tail-trim fresh reload');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''' % {'profile': profile})
    self.run_browser('a.html', '/report_result?0', timeout=240)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_checkpoint_recovery(self):
    # Exercise two alternating V4 checkpoint generations in fresh documents.
    # The parent reads only the fixed test artifacts after each orderly drain;
    # native mounts prove the logical state. Separate phase cuts establish
    # old/new selector recovery, and an open-unlink role checks that a
    # prospective orphan prevents premature source-arena reclamation. This is
    # controlled iframe-disposal evidence, not physical power-loss or
    # Chromium-profile recovery evidence.
    test = ('wasmfs/'
            'wasmfs_opfs_profile_log_v4_filesystem_checkpoint_recovery.c')
    base_args = [
      '-sWASMFS',
      '-pthread',
      '-sPROXY_TO_PTHREAD',
      '-lopfs.js',
    ]
    common_args = base_args + [
      '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL=3',
    ]

    def profile_arg(profile):
      return ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
              'PROFILE_NAME=' + profile)

    def compile_role(output, profile, role, extra_args=None):
      self.compile_btest(
        test, common_args + [profile_arg(profile), role] +
          (extra_args or []) + ['-o', output], reporting=Reporting.NONE)

    def compile_production_role(output, profile, role):
      self.compile_btest(
        test, base_args + [profile_arg(profile), role, '-o', output],
        reporting=Reporting.NONE)

    def profile(name):
      return ('wasmfs_profile_log_v4_filesystem_checkpoint_%s_%016x' %
              (name, random.getrandbits(64)))

    clean_profile = profile('clean')
    compile_role(
      'v4fs-checkpoint-clean-seed.html', clean_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED')
    compile_role(
      'v4fs-checkpoint-clean-round-one.html', clean_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_ONE')
    compile_role(
      'v4fs-checkpoint-clean-round-two.html', clean_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ROUND_TWO')
    compile_role(
      'v4fs-checkpoint-clean-verifier.html', clean_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_VERIFIER')

    production_profile = profile('production')
    compile_production_role(
      'v4fs-checkpoint-production.html', production_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_PRODUCTION')
    compile_production_role(
      'v4fs-checkpoint-production-reload.html', production_profile,
      ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
       'PRODUCTION_RELOAD'))

    phase_cases = []
    for phase in (1, 2):
      phase_profile = profile('phase-%d' % phase)
      prefix = 'v4fs-checkpoint-phase-%d' % phase
      seed = prefix + '-seed.html'
      interruptor = prefix + '-interruptor.html'
      verifier = prefix + '-verifier.html'
      reload = prefix + '-reload.html'
      compile_role(
        seed, phase_profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED')
      compile_role(
        interruptor, phase_profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_INTERRUPTOR',
        [
          ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
           'INTERRUPT_PHASE=%d' % phase),
          '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1',
        ])
      recovery_args = [
        ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
         'EXPECT_ROUND_ONE=%d' % int(phase == 2)),
      ]
      compile_role(
        verifier, phase_profile,
        ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
         'RECOVERY_VERIFIER'), recovery_args)
      compile_role(
        reload, phase_profile,
        ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
         'RECOVERY_RELOAD'), recovery_args)
      phase_cases.append((phase, phase_profile, seed, interruptor,
                          verifier, reload))

    orphan_profile = profile('orphan')
    compile_role(
      'v4fs-checkpoint-orphan-seed.html', orphan_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED')
    compile_role(
      'v4fs-checkpoint-orphan.html', orphan_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_ORPHAN')
    compile_role(
      'v4fs-checkpoint-orphan-reload.html', orphan_profile,
      ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
       'ORPHAN_RELOAD'))

    replacement_profile = profile('replacement')
    compile_role(
      'v4fs-checkpoint-replacement-seed.html', replacement_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_SEED')
    compile_role(
      'v4fs-checkpoint-replacement.html', replacement_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_REPLACEMENT')
    compile_role(
      'v4fs-checkpoint-replacement-reload.html', replacement_profile,
      ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_CHECKPOINT_TEST_' +
       'REPLACEMENT_RELOAD'))

    phase_script = '\n'.join(
      "          await runAfterLeaseRelease('%s', kSeed, '%s seed');\n"
      "          await interruptAfterLeaseRelease('%s', %d, '%s interruption');\n"
      "          const interrupted%d = await waitForPhysicalLayout(" \
      "'%s', '%s interrupted layout');\n"
      "          requireInterruptedCheckpoint(interrupted%d, %d, " \
      "'%s interruption');\n"
      "          await runAfterLeaseRelease('%s', kRecoveryVerifier, " \
      "'%s recovery verifier');\n"
      "          const recovered%d = await waitForPhysicalLayout(" \
      "'%s', '%s recovered layout');\n"
      "          requireExactPhysicalBounds(recovered%d, '%s recovery verifier');\n"
      "          await runAfterLeaseRelease('%s', kRecoveryReload, " \
      "'%s recovery reload');" %
      (seed, 'phase %d' % phase, interruptor, phase, 'phase %d' % phase,
       phase, phase_profile, 'phase %d' % phase, phase, phase,
       'phase %d' % phase, verifier, 'phase %d' % phase, phase,
       phase_profile, 'phase %d' % phase, phase, 'phase %d' % phase,
       reload, 'phase %d' % phase)
      for phase, phase_profile, seed, interruptor, verifier, reload in
      phase_cases)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 0;
        const kRoundOne = 1;
        const kRoundTwo = 2;
        const kVerifier = 3;
        const kInterruptor = 4;
        const kRecoveryVerifier = 5;
        const kRecoveryReload = 6;
        const kOrphan = 7;
        const kOrphanReload = 8;
        const kProduction = 9;
        const kProductionReload = 10;
        const kReplacement = 11;
        const kReplacementReload = 12;
        const kBusy = 16;
        const kRecordSize = 128;
        const kControlSize = 6 * kRecordSize;
        const kManifestHeaderSize = 96;
        const kFilesystemHeaderSize = 128;
        const kFilesystemExtentSize = 48;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-filesystem-checkpoint-recovery';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function require(condition, description) {
          if (!condition) {
            throw new Error(description);
          }
        }

        function requireMagic(bytes, offset, magic, description) {
          require(offset + magic.length <= bytes.byteLength,
                  description + ' is truncated');
          for (let index = 0; index < magic.length; ++index) {
            require(bytes[offset + index] === magic.charCodeAt(index),
                    description + ' has the wrong magic');
          }
        }

        function readU64(view, offset, description) {
          const value = view.getBigUint64(offset, true);
          require(value <= BigInt(Number.MAX_SAFE_INTEGER),
                  description + ' is not a safe integer');
          return Number(value);
        }

        function parsePhase(bytes, view, offset, description) {
          requireMagic(bytes, offset, 'WFSLG4P0', description);
          require(view.getUint32(offset + 8, true) === 4,
                  description + ' has the wrong version');
          require(view.getUint32(offset + 12, true) === kRecordSize,
                  description + ' has the wrong size');
          const generation = readU64(view, offset + 16, description);
          const arena = view.getUint32(offset + 24, true);
          require(generation !== 0 && arena < 2 && arena === (generation & 1),
                  description + ' is not a valid phase');
          require(view.getUint32(offset + 28, true) === 1,
                  description + ' is not clean');
          return {generation, arena};
        }

        function parseDescriptor(bytes, view, generation, arena, copy) {
          const offset = (((generation & 1) * 2 + copy) * kRecordSize);
          const description = 'descriptor ' + generation + '/' + copy;
          requireMagic(bytes, offset, 'WFSLG4D0', description);
          require(view.getUint32(offset + 8, true) === 4,
                  description + ' has the wrong version');
          require(view.getUint32(offset + 12, true) === kRecordSize,
                  description + ' has the wrong size');
          require(readU64(view, offset + 16, description) === generation &&
                  view.getUint32(offset + 24, true) === arena,
                  description + ' does not name the selected phase');
          return {
            highWater: [
              readU64(view, offset + 56, description),
              readU64(view, offset + 64, description),
            ],
            manifestOffset: readU64(view, offset + 32, description),
            manifestSize: readU64(view, offset + 40, description),
          };
        }

        function parseSelectedDescriptor(control) {
          require(control.byteLength === kControlSize,
                  'V4 control file has the wrong fixed size');
          const view = new DataView(control.buffer, control.byteOffset,
                                    control.byteLength);
          const first = parsePhase(control, view, 4 * kRecordSize, 'phase 0');
          const second = parsePhase(control, view, 5 * kRecordSize, 'phase 1');
          let selected = first;
          if (first.generation === second.generation) {
            require(first.arena === second.arena,
                    'mirrored V4 phases disagree');
          } else {
            const older = first.generation < second.generation ? first : second;
            const newer = first.generation < second.generation ? second : first;
            require(newer.generation === older.generation + 1,
                    'V4 phases do not form a recovery split');
            selected = older;
          }
          const descriptor0 = parseDescriptor(
            control, view, selected.generation, selected.arena, 0);
          const descriptor1 = parseDescriptor(
            control, view, selected.generation, selected.arena, 1);
          require(descriptor0.manifestOffset === descriptor1.manifestOffset &&
                  descriptor0.manifestSize === descriptor1.manifestSize &&
                  descriptor0.highWater[0] === descriptor1.highWater[0] &&
                  descriptor0.highWater[1] === descriptor1.highWater[1],
                  'mirrored V4 descriptors disagree');
          return {...descriptor0, generation: selected.generation,
                  arena: selected.arena};
        }

        async function readPhysicalLayout(profile) {
          // This observer never mutates OPFS. Native V4 code selects and
          // validates records; the parent witnesses physical reachability
          // only after the relevant iframe has released its profile lease.
          const root = await navigator.storage.getDirectory();
          const stem = '.wasmfs-profile-log-v4-fs-' + profile.length + '-' +
                       profile;
          async function readFile(name) {
            const handle = await root.getFileHandle(name);
            return new Uint8Array(await (await handle.getFile()).arrayBuffer());
          }
          const control = await readFile(stem + '-control');
          const selected = parseSelectedDescriptor(control);
          const arenas = await Promise.all([0, 1].map(
            (arena) => readFile(stem + '-arena-' + arena)));
          return {...selected, arenas, physical: arenas.map((arena) =>
            arena.byteLength)};
        }

        async function waitForPhysicalLayout(profile, description) {
          let lastError;
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            try {
              return await readPhysicalLayout(profile);
            } catch (error) {
              lastError = error;
              await delay(100);
            }
          }
          throw new Error(description + ' could not read test artifacts: ' +
                          lastError);
        }

        function requireExactPhysicalBounds(layout, description) {
          require(layout.physical[0] === layout.highWater[0] &&
                  layout.physical[1] === layout.highWater[1],
                  description + ' did not trim arena files to selected high water');
        }

        function requireSelectedFilesystemExtents(layout, description,
                                                  requireCheckpoint = false) {
          const bytes = layout.arenas[layout.arena];
          const outerOffset = layout.manifestOffset;
          require(outerOffset + kManifestHeaderSize + layout.manifestSize <=
                    bytes.byteLength,
                  description + ' selected outer manifest is outside its arena');
          requireMagic(bytes, outerOffset, 'WFSLG4M0', description + ' outer');
          const outer = new DataView(bytes.buffer,
                                     bytes.byteOffset + outerOffset,
                                     kManifestHeaderSize);
          require(outer.getUint32(8, true) === 4 &&
                  outer.getUint32(12, true) === kManifestHeaderSize &&
                  readU64(outer, 16, description + ' outer') ===
                    layout.generation &&
                  readU64(outer, 24, description + ' outer') ===
                    layout.manifestSize,
                  description + ' has an invalid selected outer manifest');
          const payloadOffset = outerOffset + kManifestHeaderSize;
          const payload = bytes.subarray(payloadOffset,
                                         payloadOffset + layout.manifestSize);
          require(payload.byteLength >= kFilesystemHeaderSize,
                  description + ' filesystem payload is truncated');
          const view = new DataView(payload.buffer, payload.byteOffset,
                                    payload.byteLength);
          if (String.fromCharCode(...payload.subarray(0, 8)) === 'WFSV4FS2') {
            require(!requireCheckpoint &&
                    view.getUint32(8, true) === 2 &&
                    view.getUint32(12, true) === kFilesystemHeaderSize &&
                    readU64(view, 16, description + ' filesystem') ===
                      layout.generation &&
                    readU64(view, 48, description + ' filesystem') ===
                      layout.generation - 1 &&
                    view.getUint32(56, true) ===
                      ((layout.generation - 1) & 1) &&
                    view.getUint32(60, true) > 0,
                    description + ' has an invalid filesystem delta');
            const operationOffset = readU64(
              view, 88, description + ' filesystem');
            const operationCount = readU64(
              view, 96, description + ' filesystem');
            const blobOffset = readU64(view, 104, description + ' filesystem');
            const blobSize = readU64(view, 112, description + ' filesystem');
            require(operationOffset === kFilesystemHeaderSize &&
                    operationCount > 0 &&
                    blobOffset === operationOffset + operationCount * 32 &&
                    blobOffset + blobSize === payload.byteLength,
                    description + ' has an invalid filesystem delta layout');
            return;
          }
          requireMagic(payload, 0, 'WFSV4FS1', description + ' filesystem');
          require(view.getUint32(8, true) === 1 &&
                  view.getUint32(12, true) === kFilesystemHeaderSize &&
                  readU64(view, 16, description + ' filesystem') ===
                    layout.generation,
                  description + ' has an invalid filesystem manifest');
          const extentOffset = readU64(view, 80, description + ' filesystem');
          const extentCount = readU64(view, 88, description + ' filesystem');
          require(extentOffset <= payload.byteLength &&
                  extentCount <=
                    Math.floor((payload.byteLength - extentOffset) /
                               kFilesystemExtentSize),
                  description + ' extent table is outside its manifest');
          for (let index = 0; index < extentCount; ++index) {
            const offset = extentOffset + index * kFilesystemExtentSize;
            require(view.getUint32(offset + 16, true) === layout.arena,
                    description + ' retains a live extent in the retired arena');
          }
        }

        function requireCompactedLayout(layout, generation, arena, description) {
          const retired = arena ^ 1;
          require(layout.generation === generation && layout.arena === arena,
                  description + ' selected the wrong checkpoint generation');
          require(layout.highWater[retired] === 0 &&
                  layout.physical[retired] === 0,
                  description + ' did not retire its source arena');
          requireExactPhysicalBounds(layout, description);
          requireSelectedFilesystemExtents(layout, description, true);
        }

        function requireInterruptedCheckpoint(layout, phase, description) {
          if (phase === 1) {
            require(layout.generation === 5 && layout.arena === 1,
                    description + ' did not retain the old selected state');
            require(layout.physical[0] > layout.highWater[0] &&
                    layout.physical[1] === layout.highWater[1],
                    description + ' did not retain only an unreachable target tail');
          } else {
            require(layout.generation === 6 && layout.arena === 0 &&
                    layout.highWater[1] === 0 &&
                    layout.physical[0] === layout.highWater[0] &&
                    layout.physical[1] > 0,
                    description + ' did not expose the self-contained checkpoint');
          }
          requireSelectedFilesystemExtents(layout, description, phase === 2);
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function interruptAfterLeaseRelease(path, checkpoint, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.event !== 'interrupt' ||
                message.checkpoint !== checkpoint) {
              throw new Error(description + ' did not reach phase ' +
                              checkpoint + ': role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease('v4fs-checkpoint-production.html',
                                     kProduction,
                                     'production-cadence checkpoint run');
          const production = await waitForPhysicalLayout(
            '%(production_profile)s', 'production-cadence checkpoint layout');
          requireCompactedLayout(production, 62, 0,
                                 'production-cadence checkpoint run');
          await runAfterLeaseRelease(
            'v4fs-checkpoint-production-reload.html', kProductionReload,
            'production-cadence fresh reload');
          await runAfterLeaseRelease('v4fs-checkpoint-clean-seed.html', kSeed,
                                     'clean checkpoint seed');
          await runAfterLeaseRelease('v4fs-checkpoint-clean-round-one.html',
                                     kRoundOne, 'first checkpoint');
          const first = await waitForPhysicalLayout(
            '%(clean_profile)s', 'first checkpoint layout');
          requireCompactedLayout(first, 6, 0, 'first checkpoint');
          await runAfterLeaseRelease('v4fs-checkpoint-clean-round-two.html',
                                     kRoundTwo, 'second checkpoint');
          const second = await waitForPhysicalLayout(
            '%(clean_profile)s', 'second checkpoint layout');
          requireCompactedLayout(second, 9, 1, 'second checkpoint');
          await runAfterLeaseRelease('v4fs-checkpoint-clean-verifier.html',
                                     kVerifier, 'checkpoint fresh verifier');
%(phase_script)s
          await runAfterLeaseRelease('v4fs-checkpoint-orphan-seed.html', kSeed,
                                     'orphan checkpoint seed');
          await runAfterLeaseRelease('v4fs-checkpoint-orphan.html', kOrphan,
                                     'open-unlink checkpoint boundary');
          const orphan = await waitForPhysicalLayout(
            '%(orphan_profile)s', 'open-unlink checkpoint layout');
          requireCompactedLayout(orphan, 9, 1,
                                 'open-unlink checkpoint boundary');
          await runAfterLeaseRelease('v4fs-checkpoint-orphan-reload.html',
                                     kOrphanReload,
                                     'open-unlink fresh reload');
          await runAfterLeaseRelease('v4fs-checkpoint-replacement-seed.html',
                                     kSeed, 'replacement checkpoint seed');
          await runAfterLeaseRelease('v4fs-checkpoint-replacement.html',
                                     kReplacement,
                                     'open-replacement checkpoint boundary');
          const replacement = await waitForPhysicalLayout(
            '%(replacement_profile)s',
            'open-replacement checkpoint layout');
          requireCompactedLayout(replacement, 12, 0,
                                 'open-replacement checkpoint boundary');
          await runAfterLeaseRelease(
            'v4fs-checkpoint-replacement-reload.html', kReplacementReload,
            'open-replacement fresh reload');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''' % {
      'clean_profile': clean_profile,
      'phase_script': phase_script,
      'orphan_profile': orphan_profile,
      'production_profile': production_profile,
      'replacement_profile': replacement_profile,
    })
    self.run_browser('a.html', '/report_result?0', timeout=300)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_delta_recovery(self):
    # The interval-five seed forms a broad 128-file namespace. The parent then
    # observes at most one cadence window of fresh target-mode advances until
    # it sees a self-contained Schema-1 checkpoint. One more advance must
    # select a compact Schema-2 delta pointing at that observed checkpoint.
    # The parent only reads named test artifacts after checked native drains;
    # fresh WasmFS documents perform both replays. This is not a power-loss or
    # Chromium-profile persistence claim.
    test = ('wasmfs/'
            'wasmfs_opfs_profile_log_v4_filesystem_delta_recovery.c')
    profile = ('wasmfs_profile_log_v4_filesystem_delta_recovery_%016x' %
               random.getrandbits(64))
    common_args = [
      '-sWASMFS',
      '-pthread',
      '-sPROXY_TO_PTHREAD',
      '-lopfs.js',
      '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_CHECKPOINT_INTERVAL=5',
    ]

    def profile_arg(current_profile):
      return (
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_PROFILE_NAME=' +
        current_profile)

    def compile_role(output, current_profile, role, extra_args=None):
      self.compile_btest(
        test, common_args + [profile_arg(current_profile), role] +
          (extra_args or []) + ['-o', output],
        reporting=Reporting.NONE)

    def expected_mode_arg(mode):
      return ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_' +
              'EXPECT_TARGET_MODE=0%o' % mode)

    def compile_mode_roles(prefix,
                           current_profile,
                           include_replay=True,
                           include_recovery=False):
      for mode in (0o600, 0o640):
        suffix = '%o' % mode
        if include_replay:
          compile_role(
            prefix + '-replay-' + suffix + '.html', current_profile,
            '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_REPLAY',
            [expected_mode_arg(mode)])
          compile_role(
            prefix + '-reload-' + suffix + '.html', current_profile,
            '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RELOAD',
            [expected_mode_arg(mode)])
        if include_recovery:
          compile_role(
            prefix + '-recovery-' + suffix + '.html', current_profile,
            '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_RECOVERY',
            [expected_mode_arg(mode)])
          compile_role(
            prefix + '-recovery-reload-' + suffix + '.html', current_profile,
            ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_' +
             'RECOVERY_RELOAD'), [expected_mode_arg(mode)])

    def compile_extent_mode_roles(prefix, current_profile):
      for mode in (0o600, 0o640):
        suffix = '%o' % mode
        compile_role(
          prefix + '-replay-' + suffix + '.html', current_profile,
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_REPLAY',
          [expected_mode_arg(mode)])
        compile_role(
          prefix + '-reload-' + suffix + '.html', current_profile,
          '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_RELOAD',
          [expected_mode_arg(mode)])

    compile_role(
      'v4fs-delta-seed.html', profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_SEED')
    compile_role(
      'v4fs-delta-advance.html', profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_ADVANCE')
    compile_mode_roles('v4fs-delta', profile)
    compile_role(
      'v4fs-delta-parent-corruptor.html', profile,
      ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_' +
       'PARENT_CORRUPTOR'),
      ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_PARENT_CORRUPTION=1'])

    extent_profile = (
      'wasmfs_profile_log_v4_filesystem_delta_extent_%016x' %
      random.getrandbits(64))
    compile_role(
      'v4fs-delta-extent-seed.html', extent_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_SEED')
    compile_role(
      'v4fs-delta-extent-advance.html', extent_profile,
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_EXTENT_ADVANCE')
    compile_extent_mode_roles('v4fs-delta-extent', extent_profile)
    compile_role(
      'v4fs-delta-extent-corruptor.html', extent_profile,
      ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_' +
       'EXTENT_CORRUPTOR'),
      ['-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_HISTORICAL_EXTENT_CORRUPTION=1'])

    phase_cases = []
    for phase in (1, 2):
      phase_profile = (
        'wasmfs_profile_log_v4_filesystem_delta_recovery_phase_%d_%016x' %
        (phase, random.getrandbits(64)))
      prefix = 'v4fs-delta-phase-%d' % phase
      seed = prefix + '-seed.html'
      advance = prefix + '-advance.html'
      interruptor = prefix + '-interruptor.html'
      compile_role(
        seed, phase_profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_SEED')
      compile_role(
        advance, phase_profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_ADVANCE')
      compile_role(
        interruptor, phase_profile,
        '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_INTERRUPTOR',
        [
          ('-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_DELTA_TEST_' +
           'INTERRUPT_PHASE=%d' % phase),
          '-sWASMFS_OPFS_PROFILE_LOG_V4_TEST_INTERRUPT=1',
        ])
      compile_mode_roles(prefix, phase_profile, include_replay=False,
                         include_recovery=True)
      phase_cases.append((phase, phase_profile, seed, advance, interruptor))

    phase_script = '\n'.join(
      "          await runAfterLeaseRelease('%s', kSeed, "
      "'Schema-2 phase %d checkpoint seed');\n"
      "          const phase%dCheckpoint = await prepareCheckpoint(\n"
      "            '%s', '%s', 'Schema-2 phase %d checkpoint preparation');\n"
      "          await interruptAfterLeaseRelease('%s', %d, "
      "'Schema-2 phase %d delta interruption');\n"
      "          const phase%dInterrupted = requireInterruptedDelta(\n"
      "            await waitForPhysicalLayout('%s', "
      "'Schema-2 phase %d interrupted layout'),\n"
      "            %d, phase%dCheckpoint);\n"
      "          await runAfterLeaseRelease(\n"
      "            targetModeModule('v4fs-delta-phase-%d-recovery',\n"
      "                             phase%dInterrupted.targetMode),\n"
      "            kRecovery, 'Schema-2 phase %d fresh recovery');\n"
      "          await runAfterLeaseRelease(\n"
      "            targetModeModule('v4fs-delta-phase-%d-recovery-reload',\n"
      "                             phase%dInterrupted.targetMode),\n"
      "            kRecoveryReload, 'Schema-2 phase %d recovery reload');" %
      (seed, phase, phase, phase_profile, advance, phase, interruptor, phase,
       phase, phase, phase_profile, phase, phase, phase, phase, phase, phase,
       phase, phase, phase)
      for phase, phase_profile, seed, advance, interruptor in phase_cases)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 0;
        const kAdvance = 1;
        const kReplay = 2;
        const kReload = 3;
        const kInterruptor = 4;
        const kRecovery = 5;
        const kRecoveryReload = 6;
        const kParentCorruptor = 7;
        const kExtentSeed = 8;
        const kExtentAdvance = 9;
        const kExtentReplay = 10;
        const kExtentReload = 11;
        const kExtentCorruptor = 12;
        const kBusy = 16;
        const kProfile = '__PROFILE__';
        const kExtentProfile = '__EXTENT_PROFILE__';
        const kRecordSize = 128;
        const kControlSize = 6 * kRecordSize;
        const kManifestHeaderSize = 96;
        const kFilesystemHeaderSize = 128;
        const kFilesystemInodeSize = 112;
        const kFilesystemExtentSize = 48;
        const kDataHeaderSize = 96;
        const kChunkSize = 64 * 1024;
        const kDeltaOperationSize = 32;
        const kTargetInode = 39;
        const kExpectedInodeCount = 129;
        const kExpectedNextInode = 130;
        const kTargetModes = [0o600, 0o640];
        const kExtentPayload = new Uint8Array([
          0x75, 0x18, 0xb4, 0x3e, 0x92, 0x4f, 0xd1, 0x0b,
          0xe6, 0x39, 0x5a, 0xc7, 0x21, 0x8d, 0xf0, 0x64,
          0x0e, 0xa3, 0x57, 0xcc, 0x19, 0x76, 0xe1, 0x4a,
          0xbd, 0x02, 0x98, 0x35, 0x6f, 0xd8, 0x41, 0xae,
        ]);
        const kCheckpointPreparationAttempts = 5;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-filesystem-delta-recovery';
        const kEventTimeoutMs = 30000;
        const kReleaseAttempts = 100;
        const kFnvOffset = 1469598103934665603n;
        const kFnvPrime = 1099511628211n;
        const kU64Mask = (1n << 64n) - 1n;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function require(condition, description) {
          if (!condition) {
            throw new Error(description);
          }
        }

        function requireMagic(bytes, offset, magic, description) {
          require(offset + magic.length <= bytes.byteLength,
                  description + ' is truncated');
          for (let index = 0; index < magic.length; ++index) {
            require(bytes[offset + index] === magic.charCodeAt(index),
                    description + ' has the wrong magic');
          }
        }

        function readU64(view, offset, description) {
          const value = view.getBigUint64(offset, true);
          require(value <= BigInt(Number.MAX_SAFE_INTEGER),
                  description + ' is not a safe integer');
          return Number(value);
        }

        function checksum(bytes, zeroOffset = -1, zeroLength = 0) {
          let value = kFnvOffset;
          for (let index = 0; index < bytes.byteLength; ++index) {
            const byte = index >= zeroOffset && index < zeroOffset + zeroLength
              ? 0 : bytes[index];
            value ^= BigInt(byte);
            value = (value * kFnvPrime) & kU64Mask;
          }
          return value;
        }

        function parsePhase(control, view, offset, description) {
          const record = control.subarray(offset, offset + kRecordSize);
          requireMagic(control, offset, 'WFSLG4P0', description);
          require(view.getUint32(offset + 8, true) === 4 &&
                  view.getUint32(offset + 12, true) === kRecordSize &&
                  checksum(record, 48, 8) ===
                    view.getBigUint64(offset + 48, true),
                  description + ' has an invalid phase witness');
          const generation = readU64(view, offset + 16, description);
          const arena = view.getUint32(offset + 24, true);
          require(generation !== 0 && arena < 2 && arena === (generation & 1) &&
                  view.getUint32(offset + 28, true) === 1,
                  description + ' has an invalid selected phase');
          return {
            generation,
            arena,
            descriptorChecksum: view.getBigUint64(offset + 32, true),
          };
        }

        function parseDescriptor(control, view, generation, arena, copy) {
          const offset = (((generation & 1) * 2 + copy) * kRecordSize);
          const description = 'descriptor ' + generation + '/' + copy;
          const record = control.subarray(offset, offset + kRecordSize);
          requireMagic(control, offset, 'WFSLG4D0', description);
          require(view.getUint32(offset + 8, true) === 4 &&
                  view.getUint32(offset + 12, true) === kRecordSize &&
                  readU64(view, offset + 16, description) === generation &&
                  view.getUint32(offset + 24, true) === arena &&
                  checksum(record, 88, 8) ===
                    view.getBigUint64(offset + 88, true),
                  description + ' has an invalid descriptor record');
          return {
            manifestOffset: readU64(view, offset + 32, description),
            manifestSize: readU64(view, offset + 40, description),
            manifestRecordChecksum: view.getBigUint64(offset + 48, true),
            highWater: [
              readU64(view, offset + 56, description),
              readU64(view, offset + 64, description),
            ],
            recordChecksum: view.getBigUint64(offset + 88, true),
          };
        }

        function parseSelectedDescriptor(control) {
          require(control.byteLength === kControlSize,
                  'V4 control file has the wrong fixed size');
          const view = new DataView(control.buffer, control.byteOffset,
                                    control.byteLength);
          const first = parsePhase(control, view, 4 * kRecordSize, 'phase 0');
          const second = parsePhase(control, view, 5 * kRecordSize, 'phase 1');
          let selected = first;
          if (first.generation === second.generation) {
            require(first.arena === second.arena &&
                    first.descriptorChecksum === second.descriptorChecksum,
                    'mirrored V4 phases disagree');
          } else {
            const older = first.generation < second.generation ? first : second;
            const newer = first.generation < second.generation ? second : first;
            require(newer.generation === older.generation + 1,
                    'V4 phases do not form a recovery split');
            selected = older;
          }
          const descriptor0 = parseDescriptor(
            control, view, selected.generation, selected.arena, 0);
          const descriptor1 = parseDescriptor(
            control, view, selected.generation, selected.arena, 1);
          require(descriptor0.manifestOffset === descriptor1.manifestOffset &&
                  descriptor0.manifestSize === descriptor1.manifestSize &&
                  descriptor0.manifestRecordChecksum ===
                    descriptor1.manifestRecordChecksum &&
                  descriptor0.highWater[0] === descriptor1.highWater[0] &&
                  descriptor0.highWater[1] === descriptor1.highWater[1] &&
                  descriptor0.recordChecksum === selected.descriptorChecksum &&
                  descriptor1.recordChecksum === selected.descriptorChecksum,
                  'mirrored V4 descriptors disagree');
          return {...descriptor0, generation: selected.generation,
                  arena: selected.arena};
        }

        async function readPhysicalLayout(profile) {
          // This observer never mutates OPFS.  Native V4 code owns selection,
          // validation, replay, and every write; the parent only reads fixed
          // test artifacts after the iframe has released its lease.
          const root = await navigator.storage.getDirectory();
          const stem = '.wasmfs-profile-log-v4-fs-' + profile.length + '-' +
                       profile;
          async function readFile(name) {
            const handle = await root.getFileHandle(name);
            return new Uint8Array(await (await handle.getFile()).arrayBuffer());
          }
          const control = await readFile(stem + '-control');
          const selected = parseSelectedDescriptor(control);
          const arenas = await Promise.all([0, 1].map(
            (arena) => readFile(stem + '-arena-' + arena)));
          return {...selected, arenas, physical: arenas.map(
            (arena) => arena.byteLength)};
        }

        async function waitForPhysicalLayout(profile, description) {
          let lastError;
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            try {
              return await readPhysicalLayout(profile);
            } catch (error) {
              lastError = error;
              await delay(100);
            }
          }
          throw new Error(description + ' could not read test artifacts: ' +
                          lastError);
        }

        function requireExactPhysicalBounds(layout, description) {
          require(layout.physical[0] === layout.highWater[0] &&
                  layout.physical[1] === layout.highWater[1],
                  description + ' did not trim arena files to selected high water');
        }

        function parseOuterManifest(layout, arena, offset, size, generation,
                                    expectedRecordChecksum, description) {
          require(arena < 2 && offset + kManifestHeaderSize + size <=
                    layout.highWater[arena] &&
                    offset + kManifestHeaderSize + size <=
                    layout.arenas[arena].byteLength,
                  description + ' lies outside selected V4 high water');
          const bytes = layout.arenas[arena];
          const header = bytes.subarray(offset, offset + kManifestHeaderSize);
          requireMagic(bytes, offset, 'WFSLG4M0', description + ' outer');
          const view = new DataView(header.buffer, header.byteOffset,
                                    header.byteLength);
          require(view.getUint32(8, true) === 4 &&
                  view.getUint32(12, true) === kManifestHeaderSize &&
                  readU64(view, 16, description + ' outer') === generation &&
                  readU64(view, 24, description + ' outer') === size &&
                  checksum(header, 56, 8) === view.getBigUint64(56, true) &&
                  view.getBigUint64(56, true) === expectedRecordChecksum,
                  description + ' has an invalid outer manifest header');
          const payload = bytes.subarray(offset + kManifestHeaderSize,
                                         offset + kManifestHeaderSize + size);
          require(checksum(payload) === view.getBigUint64(32, true),
                  description + ' has an invalid outer payload checksum');
          return {arena, offset, size, generation, payload,
                  recordChecksum: view.getBigUint64(56, true)};
        }

        function describeLayout(layout) {
          return 'g=' + layout.generation + ', a=' + layout.arena +
            ', highWater=' + layout.highWater + ', physical=' + layout.physical;
        }

        function hasMagic(bytes, magic) {
          if (bytes.byteLength < magic.length) {
            return false;
          }
          for (let index = 0; index < magic.length; ++index) {
            if (bytes[index] !== magic.charCodeAt(index)) {
              return false;
            }
          }
          return true;
        }

        function requireTargetMode(mode, description) {
          require(kTargetModes.includes(mode),
                  description + ' has an unexpected target mode ' +
                    mode.toString(8));
          return mode;
        }

        function requireSameSelectedTuple(layout, checkpoint, description) {
          require(layout.generation === checkpoint.generation &&
                  layout.arena === checkpoint.arena &&
                  layout.manifestOffset === checkpoint.manifestOffset &&
                  layout.manifestSize === checkpoint.manifestSize &&
                  layout.manifestRecordChecksum ===
                    checkpoint.manifestRecordChecksum &&
                  layout.recordChecksum === checkpoint.recordChecksum &&
                  layout.highWater[0] === checkpoint.highWater[0] &&
                  layout.highWater[1] === checkpoint.highWater[1],
                  description + ' changed the selected checkpoint tuple (' +
                    describeLayout(layout) + ')');
        }

        function targetModeFromFullCheckpoint(payload, description) {
          const offset = kFilesystemHeaderSize +
            (kTargetInode - 1) * kFilesystemInodeSize;
          require(offset + kFilesystemInodeSize <= payload.byteLength,
                  description + ' target inode lies outside its full checkpoint');
          const view = new DataView(payload.buffer, payload.byteOffset,
                                    payload.byteLength);
          require(readU64(view, offset, description + ' target inode') ===
                    kTargetInode &&
                  view.getUint32(offset + 8, true) === 1,
                  description + ' does not contain the expected target inode');
          return requireTargetMode(view.getUint32(offset + 16, true) & 0o777,
                                   description + ' target inode');
        }

        function requireFullCheckpoint(layout, description, expected = null,
                                       allowUnreachableTargetTail = false) {
          const retiredArena = layout.arena ^ 1;
          require(layout.generation !== 0 &&
                  layout.arena === (layout.generation & 1) &&
                  layout.highWater[retiredArena] === 0,
                  description + ' is not a selected self-contained checkpoint (' +
                    describeLayout(layout) + ')');
          if (expected) {
            requireSameSelectedTuple(layout, expected, description);
          }
          if (allowUnreachableTargetTail) {
            require(layout.physical[layout.arena] ===
                      layout.highWater[layout.arena] &&
                    layout.physical[retiredArena] >
                      layout.highWater[retiredArena],
                    description + ' did not retain only an unreachable next-arena tail');
          } else {
            requireExactPhysicalBounds(layout, description);
          }
          const outer = parseOuterManifest(
            layout, layout.arena, layout.manifestOffset, layout.manifestSize,
            layout.generation, layout.manifestRecordChecksum, description);
          const payload = outer.payload;
          const view = new DataView(payload.buffer, payload.byteOffset,
                                    payload.byteLength);
          requireMagic(payload, 0, 'WFSV4FS1', description + ' filesystem');
          require(view.getUint32(8, true) === 1 &&
                  view.getUint32(12, true) === kFilesystemHeaderSize &&
                  readU64(view, 16, description + ' filesystem') ===
                    layout.generation &&
                  readU64(view, 24, description + ' filesystem') === 1 &&
                  readU64(view, 32, description + ' filesystem') ===
                    kExpectedNextInode &&
                  view.getUint32(40, true) === 16 &&
                  view.getUint32(44, true) === 0 &&
                  readU64(view, 48, description + ' filesystem') ===
                    kFilesystemHeaderSize &&
                  readU64(view, 56, description + ' filesystem') ===
                    kExpectedInodeCount,
                  description + ' is not the expected self-contained Schema-1 tree');
          const targetMode = targetModeFromFullCheckpoint(payload, description);
          return {
            outer,
            payloadSize: payload.byteLength,
            targetMode,
            generation: layout.generation,
            arena: layout.arena,
            manifestOffset: layout.manifestOffset,
            manifestSize: layout.manifestSize,
            manifestRecordChecksum: layout.manifestRecordChecksum,
            recordChecksum: layout.recordChecksum,
            highWater: layout.highWater,
            layout,
          };
        }

        function maybeFullCheckpoint(layout, description) {
          const outer = parseOuterManifest(
            layout, layout.arena, layout.manifestOffset, layout.manifestSize,
            layout.generation, layout.manifestRecordChecksum, description);
          if (hasMagic(outer.payload, 'WFSV4FS2')) {
            return null;
          }
          requireMagic(outer.payload, 0, 'WFSV4FS1',
                       description + ' selected filesystem');
          return requireFullCheckpoint(layout, description);
        }

        async function prepareCheckpoint(profile,
                                         advancePath,
                                         description,
                                         advanceRole = kAdvance) {
          let layout;
          for (let attempt = 0; attempt <= kCheckpointPreparationAttempts;
               ++attempt) {
            layout = await waitForPhysicalLayout(profile, description);
            const checkpoint = maybeFullCheckpoint(layout, description);
            if (checkpoint) {
              return checkpoint;
            }
            if (attempt !== kCheckpointPreparationAttempts) {
              await runAfterLeaseRelease(
                advancePath, advanceRole,
                description + ' target-mode advance ' + (attempt + 1));
            }
          }
          throw new Error(description + ' did not reach a Schema-1 checkpoint (' +
                          describeLayout(layout) + ')');
        }

        function targetModeModule(prefix, targetMode) {
          return prefix + '-' + requireTargetMode(
            targetMode, prefix + ' selected mode').toString(8) + '.html';
        }

        function requireTinyDelta(layout, checkpoint, description) {
          require(layout.generation === checkpoint.generation + 1 &&
                  layout.arena === (layout.generation & 1),
                  description + ' did not select the checkpoint successor (' +
                    describeLayout(layout) + ')');
          requireExactPhysicalBounds(layout, description);
          const outer = parseOuterManifest(
            layout, layout.arena, layout.manifestOffset, layout.manifestSize,
            layout.generation, layout.manifestRecordChecksum,
            description);
          const payload = outer.payload;
          const view = new DataView(payload.buffer, payload.byteOffset,
                                    payload.byteLength);
          requireMagic(payload, 0, 'WFSV4FS2', description + ' filesystem');
          require(view.getUint32(8, true) === 2 &&
                  view.getUint32(12, true) === kFilesystemHeaderSize &&
                  readU64(view, 16, description + ' filesystem') ===
                    layout.generation &&
                  readU64(view, 24, description + ' filesystem') === 1 &&
                  readU64(view, 32, description + ' filesystem') ===
                    kExpectedNextInode &&
                  view.getUint32(40, true) === 16 &&
                  view.getUint32(44, true) === 0 &&
                  readU64(view, 48, description + ' filesystem') ===
                    checkpoint.generation &&
                  view.getUint32(56, true) === checkpoint.arena &&
                  view.getUint32(60, true) === 1,
                  'selected Schema-2 header is not a depth-one delta');
          const parentOffset = readU64(view, 64, description + ' filesystem');
          const parentSize = readU64(view, 72, description + ' filesystem');
          const parentChecksum = view.getBigUint64(80, true);
          const operationOffset = readU64(
            view, 88, description + ' filesystem');
          const operationCount = readU64(
            view, 96, description + ' filesystem');
          const blobOffset = readU64(view, 104, description + ' filesystem');
          const blobSize = readU64(view, 112, description + ' filesystem');
          require(parentOffset === checkpoint.outer.offset &&
                  parentSize === checkpoint.outer.size &&
                  parentChecksum === checkpoint.outer.recordChecksum,
                  description + ' parent locator does not name its checkpoint');
          const parent = parseOuterManifest(
            layout, checkpoint.arena, parentOffset, parentSize,
            checkpoint.generation, parentChecksum,
            description + ' parent locator');
          requireMagic(parent.payload, 0, 'WFSV4FS1',
                       description + ' parent filesystem');
          const parentView = new DataView(parent.payload.buffer,
                                           parent.payload.byteOffset,
                                           parent.payload.byteLength);
          require(parentView.getUint32(8, true) === 1 &&
                  readU64(parentView, 16, description + ' parent filesystem') ===
                    checkpoint.generation,
                  description + ' parent is not a self-contained checkpoint');
          require(operationOffset === kFilesystemHeaderSize &&
                  operationCount === 1 &&
                  blobOffset === operationOffset + kDeltaOperationSize &&
                  blobSize >= kFilesystemHeaderSize &&
                  blobOffset + blobSize === payload.byteLength,
                  'Schema-2 delta does not have one bounded inode operation');
          require(readU64(view, operationOffset, description + ' operation') ===
                    kTargetInode &&
                  view.getUint32(operationOffset + 8, true) === 1 &&
                  view.getUint32(operationOffset + 12, true) === 0 &&
                  readU64(view, operationOffset + 16,
                          description + ' operation') === blobOffset &&
                  readU64(view, operationOffset + 24, description + ' operation') ===
                    blobSize,
                  'Schema-2 delta changed more than the target inode');
          const inode = new DataView(payload.buffer,
                                     payload.byteOffset + blobOffset, blobSize);
          const targetMode = requireTargetMode(
            inode.getUint32(16, true) & 0o777,
            description + ' target inode');
          require(readU64(inode, 0, description + ' target inode') ===
                    kTargetInode &&
                  inode.getUint32(8, true) === 1 &&
                  targetMode !== checkpoint.targetMode &&
                  readU64(inode, 64, description + ' target inode') === 0 &&
                  readU64(inode, 80, description + ' target inode') === 0,
                  'Schema-2 target inode is not the tiny mode post-image');
          require(payload.byteLength * 16 < checkpoint.payloadSize,
                  description +
                    ' wire record is not materially smaller than its checkpoint');
          return {outer, payloadSize: payload.byteLength, targetMode};
        }

        function requireEqualBytes(before, after, description) {
          require(before.byteLength === after.byteLength,
                  description + ' has a different byte length');
          for (let index = 0; index < before.byteLength; ++index) {
            require(before[index] === after[index],
                    description + ' changed at byte ' + index);
          }
        }

        function requireSameSelectedDelta(before, after, checkpoint) {
          const beforeDelta = requireTinyDelta(
            before, checkpoint, 'Schema-2 delta before historical-parent fault');
          const afterDelta = requireTinyDelta(
            after, checkpoint, 'Schema-2 delta after historical-parent fault');
          requireSameSelectedTuple(
            after, before, 'historical-parent fault observer');
          const beforeWire = before.arenas[before.arena].subarray(
            before.manifestOffset,
            before.manifestOffset + kManifestHeaderSize + before.manifestSize);
          const afterWire = after.arenas[after.arena].subarray(
            after.manifestOffset,
            after.manifestOffset + kManifestHeaderSize + after.manifestSize);
          requireEqualBytes(beforeWire, afterWire,
                            'historical-parent fault selected manifest wire');
          require(beforeDelta.targetMode === afterDelta.targetMode,
                  'historical-parent fault changed the selected target mode');
          return afterDelta;
        }

        function requireHistoricalExtentCheckpoint(layout, checkpoint,
                                                   description) {
          const outer = parseOuterManifest(
            layout, checkpoint.arena, checkpoint.outer.offset,
            checkpoint.outer.size, checkpoint.generation,
            checkpoint.outer.recordChecksum, description + ' full checkpoint');
          const payload = outer.payload;
          const view = new DataView(payload.buffer, payload.byteOffset,
                                    payload.byteLength);
          requireMagic(payload, 0, 'WFSV4FS1',
                       description + ' full filesystem');
          const targetOffset = kFilesystemHeaderSize +
            (kTargetInode - 1) * kFilesystemInodeSize;
          require(targetOffset + kFilesystemInodeSize <= payload.byteLength &&
                  readU64(view, targetOffset, description + ' target inode') ===
                    kTargetInode &&
                  view.getUint32(targetOffset + 8, true) === 1 &&
                  readU64(view, targetOffset + 48,
                          description + ' target inode') ===
                    kExtentPayload.byteLength &&
                  readU64(view, targetOffset + 80,
                          description + ' target inode') === 1,
                  description + ' full checkpoint lacks the target extent');
          const extentOffset = readU64(
            view, 80, description + ' full filesystem');
          const extentCount = readU64(
            view, 88, description + ' full filesystem');
          require(extentCount === 1 &&
                  extentOffset + extentCount * kFilesystemExtentSize <=
                    payload.byteLength,
                  description + ' full checkpoint does not have one bounded extent');
          require(readU64(view, extentOffset,
                          description + ' full extent') === kTargetInode &&
                  readU64(view, extentOffset + 8,
                          description + ' full extent') === 0,
                  description + ' full checkpoint extent is not target chunk zero');
          const extent = {
            arena: view.getUint32(extentOffset + 16, true),
            payloadSize: view.getUint32(extentOffset + 20, true),
            offset: readU64(view, extentOffset + 24,
                            description + ' full extent'),
            checksum: view.getBigUint64(extentOffset + 32, true),
          };
          require(extent.arena === checkpoint.arena &&
                  extent.arena === (checkpoint.generation & 1) &&
                  extent.payloadSize === kChunkSize &&
                  extent.offset + kDataHeaderSize + extent.payloadSize <=
                    layout.highWater[extent.arena],
                  description +
                    ' historical extent does not occupy the checkpoint parity');
          const arena = layout.arenas[extent.arena];
          const header = arena.subarray(extent.offset,
                                        extent.offset + kDataHeaderSize);
          const headerView = new DataView(header.buffer, header.byteOffset,
                                          header.byteLength);
          requireMagic(arena, extent.offset, 'WFSV4DA1',
                       description + ' historical data record');
          require(headerView.getUint32(8, true) === 1 &&
                  headerView.getUint32(12, true) === kDataHeaderSize &&
                  readU64(headerView, 16,
                          description + ' historical data record') ===
                    checkpoint.generation &&
                  readU64(headerView, 24,
                          description + ' historical data record') ===
                    kTargetInode &&
                  readU64(headerView, 32,
                          description + ' historical data record') === 0 &&
                  headerView.getUint32(40, true) === kChunkSize &&
                  headerView.getUint32(44, true) === 0 &&
                  checksum(header, 56, 8) === headerView.getBigUint64(56, true) &&
                  headerView.getBigUint64(48, true) === extent.checksum,
                  description + ' historical data record is invalid');
          const data = arena.subarray(extent.offset + kDataHeaderSize,
                                      extent.offset + kDataHeaderSize +
                                        extent.payloadSize);
          require(checksum(data) === extent.checksum,
                  description + ' historical data payload checksum is invalid');
          requireEqualBytes(data.subarray(0, kExtentPayload.byteLength),
                            kExtentPayload,
                            description + ' historical data payload prefix');
          return extent;
        }

        function parseExtentDelta(layout, outer, description) {
          const payload = outer.payload;
          const view = new DataView(payload.buffer, payload.byteOffset,
                                    payload.byteLength);
          requireMagic(payload, 0, 'WFSV4FS2', description + ' filesystem');
          require(view.getUint32(8, true) === 2 &&
                  view.getUint32(12, true) === kFilesystemHeaderSize &&
                  readU64(view, 16, description + ' filesystem') ===
                    outer.generation &&
                  readU64(view, 24, description + ' filesystem') === 1 &&
                  readU64(view, 32, description + ' filesystem') ===
                    kExpectedNextInode &&
                  view.getUint32(40, true) === 16 &&
                  view.getUint32(44, true) === 0,
                  description + ' is not a valid Schema-2 filesystem record');
          const parent = {
            generation: readU64(view, 48, description + ' filesystem'),
            arena: view.getUint32(56, true),
            offset: readU64(view, 64, description + ' filesystem'),
            size: readU64(view, 72, description + ' filesystem'),
            recordChecksum: view.getBigUint64(80, true),
          };
          const depth = view.getUint32(60, true);
          const operationOffset = readU64(
            view, 88, description + ' filesystem');
          const operationCount = readU64(
            view, 96, description + ' filesystem');
          const blobOffset = readU64(view, 104, description + ' filesystem');
          const blobSize = readU64(view, 112, description + ' filesystem');
          require(parent.generation !== 0 &&
                  parent.generation < outer.generation &&
                  parent.arena < 2 &&
                  parent.arena === (parent.generation & 1) && depth !== 0 &&
                  operationOffset === kFilesystemHeaderSize &&
                  operationCount === 1 &&
                  blobOffset === operationOffset + kDeltaOperationSize &&
                  blobSize === kFilesystemHeaderSize + 40 &&
                  blobOffset + blobSize === payload.byteLength,
                  description + ' does not have one bounded extent inode delta');
          require(readU64(view, operationOffset,
                          description + ' operation') === kTargetInode &&
                  view.getUint32(operationOffset + 8, true) === 1 &&
                  view.getUint32(operationOffset + 12, true) === 0 &&
                  readU64(view, operationOffset + 16,
                          description + ' operation') === blobOffset &&
                  readU64(view, operationOffset + 24,
                          description + ' operation') === blobSize,
                  description + ' changes more than the target extent inode');
          const inode = new DataView(payload.buffer,
                                     payload.byteOffset + blobOffset, blobSize);
          const targetMode = requireTargetMode(
            inode.getUint32(16, true) & 0o777,
            description + ' target inode');
          require(readU64(inode, 0, description + ' target inode') ===
                    kTargetInode &&
                  inode.getUint32(8, true) === 1 &&
                  readU64(inode, 48, description + ' target inode') ===
                    kExtentPayload.byteLength &&
                  readU64(inode, 56, description + ' target inode') ===
                    kFilesystemHeaderSize &&
                  readU64(inode, 64, description + ' target inode') === 0 &&
                  readU64(inode, 72, description + ' target inode') ===
                    kFilesystemHeaderSize &&
                  readU64(inode, 80, description + ' target inode') === 1 &&
                  readU64(inode, 88, description + ' target inode') === 0 &&
                  readU64(inode, 96, description + ' target inode') === 0,
                  description + ' target delta post-image lacks one extent');
          const extent = {
            arena: inode.getUint32(kFilesystemHeaderSize + 8, true),
            payloadSize: inode.getUint32(kFilesystemHeaderSize + 12, true),
            offset: readU64(inode, kFilesystemHeaderSize + 16,
                            description + ' target extent'),
            checksum: inode.getBigUint64(kFilesystemHeaderSize + 24, true),
          };
          require(readU64(inode, kFilesystemHeaderSize,
                          description + ' target extent') === 0 &&
                  extent.arena < 2 && extent.payloadSize === kChunkSize,
                  description + ' target delta extent is invalid');
          return {outer, generation: outer.generation, arena: outer.arena,
                  parent, depth, targetMode, extent};
        }

        function requireParentReference(delta, parent, description) {
          require(delta.parent.generation === parent.generation &&
                  delta.parent.arena === parent.arena &&
                  delta.parent.offset === parent.outer.offset &&
                  delta.parent.size === parent.outer.size &&
                  delta.parent.recordChecksum === parent.outer.recordChecksum,
                  description + ' does not name its expected parent record');
        }

        function requireSameExtent(actual, expected, description) {
          require(actual.arena === expected.arena &&
                  actual.payloadSize === expected.payloadSize &&
                  actual.offset === expected.offset &&
                  actual.checksum === expected.checksum,
                  description + ' did not retain the historical extent');
        }

        function requireExtentChronology(layout, checkpoint, description) {
          requireExactPhysicalBounds(layout, description);
          const fullExtent = requireHistoricalExtentCheckpoint(
            layout, checkpoint, description);
          const selected = parseOuterManifest(
            layout, layout.arena, layout.manifestOffset, layout.manifestSize,
            layout.generation, layout.manifestRecordChecksum,
            description + ' selected delta');
          const final = parseExtentDelta(layout, selected,
                                         description + ' second delta');
          require(final.generation === checkpoint.generation + 2 &&
                  final.arena === checkpoint.arena &&
                  final.depth >= 2 && final.depth === 2 &&
                  final.extent.arena === (final.generation & 1) &&
                  fullExtent.arena === (final.generation & 1),
                  description +
                    ' lacks a depth-two historical extent on selected parity');
          const firstOuter = parseOuterManifest(
            layout, final.parent.arena, final.parent.offset, final.parent.size,
            final.parent.generation, final.parent.recordChecksum,
            description + ' first delta parent');
          const first = parseExtentDelta(layout, firstOuter,
                                         description + ' first delta');
          require(first.generation === checkpoint.generation + 1 &&
                  first.depth === 1,
                  description + ' does not retain the first Schema-2 successor');
          requireParentReference(first, checkpoint,
                                 description + ' first delta');
          requireParentReference(final, first,
                                 description + ' second delta');
          requireSameExtent(first.extent, fullExtent,
                            description + ' first delta');
          requireSameExtent(final.extent, fullExtent,
                            description + ' second delta');
          return {fullExtent, first, final};
        }

        function requireSameSelectedExtentChronology(before, after, checkpoint) {
          const beforeChronology = requireExtentChronology(
            before, checkpoint,
            'historical-extent fault selected chronology before factory');
          const afterChronology = requireExtentChronology(
            after, checkpoint,
            'historical-extent fault selected chronology after factory');
          requireSameSelectedTuple(after, before,
                                   'historical-extent fault observer');
          require(before.physical[0] === after.physical[0] &&
                  before.physical[1] === after.physical[1],
                  'historical-extent fault changed physical arena sizes');
          const beforeWire = before.arenas[before.arena].subarray(
            before.manifestOffset,
            before.manifestOffset + kManifestHeaderSize + before.manifestSize);
          const afterWire = after.arenas[after.arena].subarray(
            after.manifestOffset,
            after.manifestOffset + kManifestHeaderSize + after.manifestSize);
          requireEqualBytes(beforeWire, afterWire,
                            'historical-extent fault selected manifest wire');
          require(beforeChronology.final.targetMode ===
                    afterChronology.final.targetMode,
                  'historical-extent fault changed the selected target mode');
          return afterChronology;
        }

        function requireInterruptedDelta(layout, phase, checkpoint) {
          if (phase === 1) {
            // The first V4 witness does not expose the new descriptor.  The
            // just-written Schema-2 bytes may remain only as an unreachable
            // next-arena tail; native recovery must choose the old FS1 tree.
            requireFullCheckpoint(layout, 'Schema-2 phase-1 interruption',
                                  checkpoint, true);
            return checkpoint;
          }
          require(phase === 2, 'unknown Schema-2 interruption phase');
          // Both witnesses make the immediate Schema-2 successor
          // authoritative. Its parent must still be reachable, so this
          // validates the selected delta before the interrupted document is
          // discarded.
          return requireTinyDelta(layout, checkpoint,
                                  'Schema-2 phase-2 interruption');
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        async function interruptAfterLeaseRelease(path, checkpoint, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              // The interruptor deliberately cannot drain.  Disposing only
              // this fresh iframe supplies the recovery boundary; subsequent
              // native roles retry until its profile lease has been released.
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.event !== 'interrupt' ||
                message.checkpoint !== checkpoint) {
              throw new Error(description + ' did not reach V4 phase ' +
                              checkpoint + ': role=' + message.role +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease('v4fs-delta-seed.html', kSeed,
                                     'Schema-2 broad checkpoint seed');
          const checkpoint = await prepareCheckpoint(
            kProfile, 'v4fs-delta-advance.html',
            'Schema-2 broad checkpoint preparation');
          await runAfterLeaseRelease('v4fs-delta-advance.html', kAdvance,
                                     'Schema-2 one-inode mutation');
          const selectedDeltaBeforeFault = await waitForPhysicalLayout(
            kProfile, 'Schema-2 selected delta before historical-parent fault');
          const delta = requireTinyDelta(
            selectedDeltaBeforeFault,
            checkpoint, 'Schema-2 selected delta');
          await runAfterLeaseRelease(
            'v4fs-delta-parent-corruptor.html', kParentCorruptor,
            'Schema-2 historical-parent fault rejects fresh mount EIO');
          const selectedDeltaAfterFault = await waitForPhysicalLayout(
            kProfile, 'Schema-2 selected delta after historical-parent fault');
          requireSameSelectedDelta(selectedDeltaBeforeFault,
                                   selectedDeltaAfterFault, checkpoint);
          await runAfterLeaseRelease(
            targetModeModule('v4fs-delta-replay', delta.targetMode), kReplay,
            'Schema-2 clean fresh native replay after historical-parent fault');
          await runAfterLeaseRelease(
            targetModeModule('v4fs-delta-reload', delta.targetMode), kReload,
            'Schema-2 second fresh native reload');
          await runAfterLeaseRelease('v4fs-delta-extent-seed.html',
                                     kExtentSeed,
                                     'Schema-2 historical-extent seed');
          const extentCheckpoint = await prepareCheckpoint(
            kExtentProfile, 'v4fs-delta-extent-advance.html',
            'Schema-2 historical-extent checkpoint preparation',
            kExtentAdvance);
          requireHistoricalExtentCheckpoint(
            extentCheckpoint.layout, extentCheckpoint,
            'Schema-2 historical-extent full checkpoint');
          await runAfterLeaseRelease('v4fs-delta-extent-advance.html',
                                     kExtentAdvance,
                                     'Schema-2 historical-extent first delta');
          await runAfterLeaseRelease('v4fs-delta-extent-advance.html',
                                     kExtentAdvance,
                                     'Schema-2 historical-extent second delta');
          const extentBeforeFault = await waitForPhysicalLayout(
            kExtentProfile, 'Schema-2 historical-extent selected chronology');
          const extentChronology = requireExtentChronology(
            extentBeforeFault, extentCheckpoint,
            'Schema-2 historical-extent selected chronology');
          await runAfterLeaseRelease(
            'v4fs-delta-extent-corruptor.html', kExtentCorruptor,
            'Schema-2 historical-extent fault rejects fresh mount EIO');
          const extentAfterFault = await waitForPhysicalLayout(
            kExtentProfile,
            'Schema-2 historical-extent chronology after fault');
          requireSameSelectedExtentChronology(
            extentBeforeFault, extentAfterFault, extentCheckpoint);
          await runAfterLeaseRelease(
            targetModeModule('v4fs-delta-extent-replay',
                             extentChronology.final.targetMode),
            kExtentReplay,
            'Schema-2 clean historical-extent binary replay');
          await runAfterLeaseRelease(
            targetModeModule('v4fs-delta-extent-reload',
                             extentChronology.final.targetMode),
            kExtentReload,
            'Schema-2 historical-extent replay reload');
__PHASE_SCRIPT__
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    '''.replace('__PROFILE__', profile).replace(
      '__EXTENT_PROFILE__', extent_profile).replace('__PHASE_SCRIPT__',
                                                   phase_script))
    self.run_browser('a.html', '/report_result?0', timeout=600)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_quota_recovery(self):
    # The quota role maps a test-only browser QuotaExceededError to ENOSPC on
    # a real V4 data transaction. It must not report successful persistence,
    # and its poisoned iframe is discarded before fresh normal documents prove
    # that the last selected state survives and remains writable. This is not
    # physical quota, power-loss, database, or Chromium-profile evidence.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem_quota_recovery.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = ('wasmfs_profile_log_v4_filesystem_quota_recovery_%016x' %
               random.getrandbits(64))
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_PROFILE_NAME=' +
      profile)

    def compile_role(output, role, extra_args=None):
      self.compile_btest(
        test, common_args + [profile_arg, role] + (extra_args or []) +
          ['-o', output], reporting=Reporting.NONE)

    compile_role(
      'v4fs-quota-seed.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_SEED')
    compile_role(
      'v4fs-quota-failure.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_QUOTA',
      ['-sWASMFS_OPFS_TEST_QUOTA_WRITE=1'])
    compile_role(
      'v4fs-quota-verifier.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_VERIFIER')
    compile_role(
      'v4fs-quota-reload.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_QUOTA_RECOVERY_TEST_RELOAD')

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kSeed = 0;
        const kQuota = 1;
        const kVerifier = 2;
        const kReload = 3;
        const kBusy = 16;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v4-filesystem-quota-recovery';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              // The quota role cannot complete a clean drain of a terminally
              // latched backend.
              // Removing only its iframe creates the fresh-document recovery
              // boundary; all ordinary roles perform a checked native drain.
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            if (message.role !== role || message.error !== 0) {
              throw new Error(description + ' failed: role=' + message.role +
                              ', stage=' + message.stage +
                              ', errno=' + message.error);
            }
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          await runAfterLeaseRelease('v4fs-quota-seed.html', kSeed,
                                     'V4 quota recovery seed');
          await runAfterLeaseRelease('v4fs-quota-failure.html', kQuota,
                                     'V4 quota ENOSPC and terminal latch');
          await runAfterLeaseRelease('v4fs-quota-verifier.html', kVerifier,
                                     'V4 fresh state after quota failure');
          await runAfterLeaseRelease('v4fs-quota-reload.html', kReload,
                                     'V4 post-quota fresh reload');
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v4_filesystem_locks(self):
    # Chromium database files use process-owned fcntl ranges. Exercise the V4
    # filesystem's leased single-process subset in separate documents: another
    # profile instance is rejected while a holder owns the Web Lock, then both
    # document teardown and explicit drain release it for a fresh lock user.
    test = 'wasmfs/wasmfs_opfs_profile_log_v4_filesystem_locks.c'
    common_args = [
      '-sWASMFS',
      '-pthread',
      '-sPROXY_TO_PTHREAD',
      '-lopfs.js',
      '-sWASMFS_RECORD_LOCK_TEST=1',
    ]
    profile = 'wasmfs_profile_log_v4_locks_%016x' % random.getrandbits(64)
    profile_arg = (
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_PROFILE_NAME=' +
      profile)

    def compile_role(output, role):
      self.compile_btest(
        test,
        common_args + [profile_arg, role, '-o', output],
        reporting=Reporting.NONE)

    compile_role(
      'v4fs-lock-holder.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_HOLDER')
    compile_role(
      'v4fs-lock-contender.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_CONTENDER')
    compile_role(
      'v4fs-lock-drainer.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_DRAINER')
    compile_role(
      'v4fs-lock-verifier.html',
      '-DWASMFS_OPFS_PROFILE_LOG_V4_FILESYSTEM_LOCK_TEST_VERIFIER')

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kDrainer = 2;
        const kVerifier = 3;
        const kBusy = 16;
        const kWitnessType = 'wasmfs-opfs-profile-log-v4-filesystem-locks';
        const kEventTimeoutMs = 25000;
        const kReleaseAttempts = 80;
        const pending = new Map();

        function delay(milliseconds) {
          return new Promise((resolve) => setTimeout(resolve, milliseconds));
        }

        function launchModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          const module = {frame, events: [], waiters: []};
          pending.set(frame.contentWindow, module);
          frame.src = path;
          return module;
        }

        function disposeModule(module) {
          pending.delete(module.frame.contentWindow);
          module.frame.remove();
        }

        function waitFor(module, description) {
          if (module.events.length) {
            return Promise.resolve(module.events.shift());
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              const index = module.waiters.findIndex(
                (waiter) => waiter.resolve === resolve);
              if (index >= 0) {
                module.waiters.splice(index, 1);
              }
              reject(new Error('timed out waiting for ' + description));
            }, kEventTimeoutMs);
            module.waiters.push({resolve, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin !== window.location.origin ||
              event.data?.type !== kWitnessType) {
            return;
          }
          const module = pending.get(event.source);
          if (!module) {
            return;
          }
          if (module.waiters.length) {
            const waiter = module.waiters.shift();
            clearTimeout(waiter.timeout);
            waiter.resolve(event.data);
          } else {
            module.events.push(event.data);
          }
        });

        function requireMessage(message, role, event, description) {
          if (message.role !== role || message.error !== 0 ||
              message.event !== event) {
            throw new Error(description + ' failed: role=' + message.role +
                            ', event=' + message.event +
                            ', errno=' + message.error);
          }
        }

        async function runAfterLeaseRelease(path, role, description) {
          for (let attempt = 0; attempt < kReleaseAttempts; ++attempt) {
            const module = launchModule(path);
            let message;
            try {
              message = await waitFor(module, description);
            } finally {
              disposeModule(module);
            }
            if (message.error === kBusy) {
              await delay(100);
              continue;
            }
            requireMessage(message, role, 'result', description);
            return;
          }
          throw new Error(description + ' never acquired the released lease');
        }

        (async () => {
          let holder;
          let drainer;
          try {
            holder = launchModule('v4fs-lock-holder.html');
            requireMessage(await waitFor(holder, 'V4 lock holder'), kHolder,
                           'ready', 'V4 lock holder');

            const contender = launchModule('v4fs-lock-contender.html');
            try {
              requireMessage(await waitFor(contender, 'V4 lock contender'),
                             kContender, 'result', 'V4 lock contender');
            } finally {
              disposeModule(contender);
            }

            // This is a crash-boundary/iframe-disposal probe. The later
            // drainer case below is the orderly Chromium-shutdown path.
            disposeModule(holder);
            holder = undefined;
            await runAfterLeaseRelease('v4fs-lock-verifier.html', kVerifier,
                                       'V4 lock verifier after disposal');

            drainer = launchModule('v4fs-lock-drainer.html');
            requireMessage(await waitFor(drainer, 'V4 explicit drainer'),
                           kDrainer, 'ready', 'V4 explicit drainer');
            // The drainer remains alive with its old descriptor table after a
            // successful detach/flush/close/release. A fresh module must still
            // obtain the lease and take its own database locks.
            await runAfterLeaseRelease('v4fs-lock-verifier.html', kVerifier,
                                       'V4 lock verifier after explicit drain');
            reportResultToServer('0');
          } finally {
            if (drainer) {
              disposeModule(drainer);
            }
            if (holder) {
              disposeModule(holder);
            }
          }
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=180)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_profile_log_v2_proxy_completion_failure(self):
    # A test-only terminal latch after a successfully flushed inactive V2 root
    # must make the explicit failed drain issue no later Worker proxy. It
    # models loss of the native acknowledgement, not a literal ProxyWorker
    # failure, and cannot observe C++ destruction after iframe disposal. This
    # remains a controlled acknowledgement test, not a physical crash model or
    # complete Chromium-profile proof.
    test = 'wasmfs/wasmfs_opfs_profile_log_v2_proxy_completion_failure.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    profile = f'wasmfs_profile_log_v2_proxy_{random.getrandbits(64):016x}'
    profile_arg = '-DWASMFS_OPFS_PROFILE_LOG_V2_PROXY_PROFILE_NAME=' + profile

    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_LOG_V2_PROXY_HOLDER',
        '-sWASMFS_OPFS_PROFILE_LOG_V2_TEST_PROXY_COMPLETION_FAILURE=1',
        '-o', 'v2-proxy-holder.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_LOG_V2_PROXY_CONTENDER',
        '-o', 'v2-proxy-contender.html',
      ],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + [
        profile_arg,
        '-DWASMFS_OPFS_PROFILE_LOG_V2_PROXY_VERIFIER',
        '-o', 'v2-proxy-verifier.html',
      ],
      reporting=Reporting.NONE)

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kVerifier = 2;
        const kTerminalFailure = 0;
        const kBusy = 1;
        const kRecoveredOldRoot = 2;
        const kEBusy = 16;
        const kWitnessType =
          'wasmfs-opfs-profile-log-v2-proxy-completion';
        const kTimeoutMs = 20000;
        const pending = new Map();

        function launch(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pending.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kTimeoutMs);
            pending.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kWitnessType ||
              event.data.event !== 'result') {
            return;
          }
          const item = pending.get(event.source);
          if (!item) {
            return;
          }
          pending.delete(event.source);
          clearTimeout(item.timeout);
          item.resolve({frame: item.frame, message: event.data});
        });

        function expect(module, role, result, description) {
          if (module.message.role !== role || module.message.result !== result ||
              module.message.error !== 0) {
            throw new Error(description + ' failed: role=' +
                            module.message.role + ', result=' +
                            module.message.result + ', errno=' +
                            module.message.error);
          }
        }

        async function verifyAfterDispose() {
          for (let attempt = 0; attempt < 80; ++attempt) {
            const verifier = await launch('v2-proxy-verifier.html');
            try {
              if (verifier.message.error === kEBusy) {
                await new Promise((resolve) => setTimeout(resolve, 100));
                continue;
              }
              expect(verifier, kVerifier, kRecoveredOldRoot,
                     'fresh old-root verifier');
              return;
            } finally {
              verifier.frame.remove();
            }
          }
          throw new Error('fresh verifier never acquired released holder lock');
        }

        (async () => {
          const holder = await launch('v2-proxy-holder.html');
          try {
            expect(holder, kHolder, kTerminalFailure,
                   'terminal no-proxy holder');
            const contender = await launch('v2-proxy-contender.html');
            try {
              expect(contender, kContender, kBusy,
                     'contender while terminal holder lives');
            } finally {
              contender.frame.remove();
            }
          } finally {
            // The failed holder deliberately owns a worker/lease tombstone;
            // document disposal is the only release path being exercised.
            holder.frame.remove();
          }
          await verifyAfterDispose();
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_profile_drain(self):
    # This is deliberately narrower than browser-profile shutdown. It checks
    # the leased-OPFS backend handoff primitive: target-only sealing/detach,
    # libc-flush reentry rejection, a worker-retirement acknowledgement, and
    # an EXIT_RUNTIME global-destruction tail that must not block browser main.
    test = test_file('wasmfs/wasmfs_opfs_profile_drain.c')
    common_args = ['-sWASMFS', '-sFORCE_FILESYSTEM', '-pthread',
                   '-sPROXY_TO_PTHREAD', '-sPTHREAD_POOL_SIZE=4',
                   '-sEXIT_RUNTIME', '-sALLOW_BLOCKING_ON_MAIN_THREAD=0',
                   '-lopfs.js']
    create_file('profile-drain-normal-holder-pre.js', r'''
      // onExit runs after the native atexit witness. The parent waits for it
      // before disposing the iframe, so this checks native global WasmFS
      // destruction rather than browser-context cleanup.
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-profile-drain',
          },
          window.location.origin);
      };
      Module['onAbort'] = (reason) => {
        if (typeof window == 'undefined') {
          console.error('profile-drain holder abort: ' + reason);
          return;
        }
        window.parent.postMessage(
          {
            event: 'holder-abort',
            reason: String(reason),
            type: 'wasmfs-opfs-profile-drain',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_NORMAL',
                     '-sWASMFS_OPFS_PROFILE_DRAIN_TEST=1', '--pre-js',
                     'profile-drain-normal-holder-pre.js', '-o',
                     'profile-drain-normal-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_NORMAL', '-o',
                     'profile-drain-normal-contender.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_NO_MOUNT', '-o',
                     'profile-drain-no-mount-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_NO_MOUNT', '-o',
                     'profile-drain-no-mount-contender.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_CLOSE_FAILURE',
                     '-sWASMFS_OPFS_TEST_CLOSE_FAILURE=1', '-o',
                     'profile-drain-close-during-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_CLOSE_FAILURE', '-o',
                     'profile-drain-close-during-contender.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_CLOSE_BEFORE',
                     '-sWASMFS_OPFS_TEST_CLOSE_FAILURE=1', '-o',
                     'profile-drain-close-before-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_CLOSE_BEFORE', '-o',
                     'profile-drain-close-before-contender.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_RELEASE_FAILURE',
                     '-sWASMFS_OPFS_TEST_LEASE_RELEASE_FAILURE=1', '-o',
                     'profile-drain-release-failure-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_RELEASE_FAILURE', '-o',
                     'profile-drain-release-failure-contender.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_REENTRY', '-o',
                     'profile-drain-reentry-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_REENTRY', '-o',
                     'profile-drain-reentry-contender.html'],
      reporting=Reporting.NONE)
    # The injected error occurs only after the one worker-side transaction has
    # acknowledged Web Locks release, reset OPFS state, and stopped heartbeat.
    # The holder must report non-success yet still reach normal EXIT_RUNTIME
    # onExit without a browser-main blocking abort.
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_RETIRE_FAILURE',
                     '-sWASMFS_OPFS_TEST_RETIRE_FAILURE=1', '--pre-js',
                     'profile-drain-normal-holder-pre.js', '-o',
                     'profile-drain-retire-failure-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_RETIRE_FAILURE', '-o',
                     'profile-drain-retire-failure-contender.html'],
      reporting=Reporting.NONE)
    # This hook rejects the browser-main no-pool fence before it touches the
    # dedicated worker. The holder must retain its lock but still reach an
    # orderly EXIT_RUNTIME tail without a browser-main join.
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_HOLDER',
                     '-DWASMFS_OPFS_PROFILE_DRAIN_FENCE_FAILURE',
                     '-sWASMFS_OPFS_TEST_RETIRE_FENCE_FAILURE=1', '--pre-js',
                     'profile-drain-normal-holder-pre.js', '-o',
                     'profile-drain-fence-failure-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_PROFILE_DRAIN_FENCE_FAILURE', '-o',
                     'profile-drain-fence-failure-contender.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kReady = 0;
        const kBusy = 1;
        const kFailure = 2;
        const kModuleTimeoutMs = 20000;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();
        const retirementTrace = new BroadcastChannel(
          'wasmfs-opfs-profile-drain-retirement');
        let heartbeatStopped = false;
        let workerNotPooled = false;
        let retirementTraceError = null;

        retirementTrace.onmessage = (event) => {
          const trace = event.data;
          if (trace?.type != 'wasmfs-opfs-profile-drain-retirement') {
            return;
          }
          if (trace.phase == 'heartbeat-stopped') {
            heartbeatStopped = true;
          } else if (trace.phase == 'worker-not-pooled') {
            if (trace.inUnusedWorkers) {
              retirementTraceError =
                'retired OPFS worker appeared in PThread.unusedWorkers';
            }
            workerNotPooled = true;
          } else if (trace.phase == 'heartbeat-tick' && heartbeatStopped) {
            retirementTraceError =
              'retired OPFS worker emitted a stale heartbeat tick';
          }
        };

        async function waitForRetirementWitness() {
          const deadline = performance.now() + kModuleTimeoutMs;
          while (!retirementTraceError &&
                 (!heartbeatStopped || !workerNotPooled) &&
                 performance.now() < deadline) {
            await new Promise((resolve) => setTimeout(resolve, 10));
          }
          if (retirementTraceError) {
            throw new Error(retirementTraceError);
          }
          if (!heartbeatStopped || !workerNotPooled) {
            throw new Error('timed out waiting for OPFS worker retirement ' +
                            'heartbeat/no-pool witnesses');
          }
          // The normal holder creates/joins a replacement pthread before it
          // reports ready. Leave time for a stale interval to surface after
          // that deterministic churn before accepting the global-exit tail.
          await new Promise((resolve) => setTimeout(resolve, 150));
          if (retirementTraceError) {
            throw new Error(retirementTraceError);
          }
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              let progress = 'unavailable';
              try {
                const getProgress =
                  frame.contentWindow.Module
                    ._wasmfs_opfs_profile_drain_holder_progress;
                if (getProgress) {
                  progress = getProgress();
                }
              } catch {}
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path +
                               ', progress=' + progress));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for orderly holder exit'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {frame, resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-profile-drain') {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status != 0) {
              pending.reject(new Error(
                'holder did not exit cleanly: status=' + event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          if (event.data.event == 'holder-abort') {
            const pendingExit = pendingHolderExits.get(event.source);
            if (pendingExit) {
              pendingHolderExits.delete(event.source);
              clearTimeout(pendingExit.timeout);
              pendingExit.frame.remove();
              pendingExit.reject(new Error(
                'holder aborted: ' + event.data.reason));
              return;
            }
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          if (event.data.event == 'holder-abort') {
            pendingModules.delete(event.source);
            clearTimeout(pending.timeout);
            pending.frame.remove();
            pending.reject(new Error('holder aborted: ' + event.data.reason));
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runScenario(name,
                                   holderPath,
                                   contenderPath,
                                   expectBusy,
                                   orderlyShutdown,
                                   keepFrames = false) {
          const holder = await startModule(holderPath);
          if (holder.message.role != kHolder ||
              holder.message.result != kReady || holder.message.error != 0) {
            holder.frame.remove();
            throw new Error(name + ' holder failed: role=' +
                            holder.message.role + ', result=' +
                            holder.message.result + ', errno=' +
                            holder.message.error);
          }

          // This fresh module runs while the holder iframe remains alive. A
          // ready result therefore proves explicit scoped release, while busy
          // proves an ambiguous close/reentrant cleanup retained the lease.
          const contender = await startModule(contenderPath);
          if (contender.message.role != kContender) {
            contender.frame.remove();
            holder.frame.remove();
            throw new Error(name + ' contender reported role ' +
                            contender.message.role);
          }
          if (expectBusy) {
            if (contender.message.result != kBusy ||
                contender.message.error == 0) {
              contender.frame.remove();
              holder.frame.remove();
              throw new Error(name +
                              ' contender acquired despite retained lease: ' +
                              'result=' + contender.message.result +
                              ', errno=' + contender.message.error);
            }
          } else if (contender.message.result != kReady ||
                     contender.message.error != 0) {
            contender.frame.remove();
            holder.frame.remove();
            throw new Error(name +
                            ' contender did not acquire before holder exit: ' +
                            'result=' + contender.message.result +
                            ', errno=' + contender.message.error);
          }
          if (orderlyShutdown) {
            const holderExit = waitForHolderExit(holder.frame);
            holder.frame.contentWindow.Module
              ._wasmfs_opfs_profile_drain_holder_shutdown();
            await holderExit;
          }
          if (keepFrames) {
            return {holder, contender};
          }
          contender.frame.remove();
          holder.frame.remove();
        }

        async function runPostReleaseRetirementFailure() {
          const holder = await startModule(
            'profile-drain-retire-failure-holder.html');
          if (holder.message.role != kHolder ||
              holder.message.result != kFailure || holder.message.error == 0) {
            holder.frame.remove();
            throw new Error('post-release retirement failure holder did not ' +
                            'report its structured non-success');
          }

          // The injected failure is after the Web Lock acknowledgement. A
          // fresh contender therefore still acquires while the failed holder
          // stays live, proving callers must use backend_retired rather than
          // treating lease_released alone as a safe result.
          const contender = await startModule(
            'profile-drain-retire-failure-contender.html');
          if (contender.message.role != kContender ||
              contender.message.result != kReady ||
              contender.message.error != 0) {
            contender.frame.remove();
            holder.frame.remove();
            throw new Error('post-release retirement failure contender did ' +
                            'not reacquire the released lock');
          }

          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_profile_drain_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
        }

        (async () => {
          // Keep the normal holder alive through the no-pool/quiet-period
          // witness. Removing its iframe first would terminate a stale worker
          // and make the 150ms heartbeat check meaningless.
          const normal = await runScenario(
            'normal scoped drain', 'profile-drain-normal-holder.html',
            'profile-drain-normal-contender.html', false, false, true);
          try {
            if (normal.holder.frame.contentWindow.Module
                  ._wasmfs_opfs_profile_drain_browser_main_attempt() != 0) {
              throw new Error('scoped drain did not reject browser-main call');
            }
            await waitForRetirementWitness();
          } finally {
            const holderExit = waitForHolderExit(normal.holder.frame);
            normal.holder.frame.contentWindow.Module
              ._wasmfs_opfs_profile_drain_holder_shutdown();
            await holderExit;
            normal.contender.frame.remove();
            normal.holder.frame.remove();
          }
          await runScenario('no-mount cleanup',
                            'profile-drain-no-mount-holder.html',
                            'profile-drain-no-mount-contender.html', false, false);
          await runScenario('close during scoped drain',
                            'profile-drain-close-during-holder.html',
                            'profile-drain-close-during-contender.html', true, false);
          await runScenario('close before scoped drain',
                            'profile-drain-close-before-holder.html',
                            'profile-drain-close-before-contender.html', true, false);
          await runScenario('lease release acknowledgement failure',
                            'profile-drain-release-failure-holder.html',
                            'profile-drain-release-failure-contender.html',
                            true, false);
          await runScenario('stdio reentry during scoped drain',
                            'profile-drain-reentry-holder.html',
                            'profile-drain-reentry-contender.html', true, false);
          await runScenario('browser-main fence failure',
                            'profile-drain-fence-failure-holder.html',
                            'profile-drain-fence-failure-contender.html',
                            true, true);
          await runPostReleaseRetirementFailure();
          retirementTrace.close();
          reportResultToServer('0');
        })().catch((error) => {
          retirementTrace.close();
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=120)

  @only_chromium
  @no_wasm64()
  def test_wasmfs_opfs_move_interrupt(self):
    # This is intentionally narrower than an OPFS atomicity, browser-crash,
    # renderer-crash, SQLite, or LevelDB recovery test. It disposes an iframe
    # while the generated ProxyWorker callback is deliberately pending at one
    # side of the actual move() call, then checks the resulting namespace from
    # a fresh leased module without letting page JavaScript access OPFS.
    test = 'wasmfs/wasmfs_opfs_move_interrupt.c'
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-lopfs.js']
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_MOVE_INTERRUPT_OWNER', '-o', 'owner.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_MOVE_INTERRUPT_MUTATOR',
                     '-sWASMFS_OPFS_TEST_MOVE_INTERRUPT=1',
                     '-o', 'mutator-before.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_MOVE_INTERRUPT_MUTATOR',
                     '-sWASMFS_OPFS_TEST_MOVE_INTERRUPT=2',
                     '-o', 'mutator-after.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_MOVE_INTERRUPT_VERIFIER',
                     '-o', 'verifier.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        // This is a controlled pending ProxyWorker callback interruption
        // only. It does not claim FileSystemFileHandle.move() atomicity,
        // browser or renderer crash recovery, or SQLite/LevelDB/database
        // recovery semantics.
        const kOwner = 0;
        const kMutator = 1;
        const kVerifier = 2;
        const kReady = 0;
        const kBusy = 1;
        const kPreMove = 2;
        const kPostMove = 3;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const kWitnessType = 'wasmfs-opfs-test-move-interrupt';
        const pendingModules = new Map();
        const pendingWitnesses = [];
        let witnessWaiter = undefined;
        const witnessChannel = new BroadcastChannel(
          'wasmfs-opfs-test-move-interrupt');

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        async function startLeasedModule(path, role, description) {
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const candidate = await startModule(path);
            if (candidate.message.role != role) {
              candidate.frame.remove();
              throw new Error(description + ' reported role ' +
                              candidate.message.role + ', expected ' + role);
            }
            if (candidate.message.result != kBusy) {
              return candidate;
            }
            candidate.frame.remove();
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(description +
                          ' did not acquire a fresh OPFS profile lease');
        }

        function settleWitness(witness) {
          if (witnessWaiter && witnessWaiter.phase == witness.phase) {
            const {resolve, timeout} = witnessWaiter;
            witnessWaiter = undefined;
            clearTimeout(timeout);
            resolve(witness);
          } else {
            pendingWitnesses.push(witness);
          }
        }

        function waitForWitness(phase) {
          const index = pendingWitnesses.findIndex(
            (witness) => witness.phase == phase);
          if (index >= 0) {
            return Promise.resolve(pendingWitnesses.splice(index, 1)[0]);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              witnessWaiter = undefined;
              reject(new Error('timed out waiting for ' + phase +
                               ' move interruption witness'));
            }, kModuleTimeoutMs);
            witnessWaiter = {phase, resolve, timeout};
          });
        }

        witnessChannel.onmessage = (event) => {
          if (event.data?.type == kWitnessType &&
              (event.data.phase == 'before' || event.data.phase == 'after')) {
            settleWitness(event.data);
          }
        };

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-move-interrupt' ||
              event.data.event != 'result') {
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function runInterruptionCase(mutatorPath, phase, disposition) {
          const owner = await startLeasedModule('owner.html', kOwner, 'owner');
          if (owner.message.result != kReady || owner.message.error != 0) {
            owner.frame.remove();
            throw new Error('owner failed: result=' + owner.message.result +
                            ', errno=' + owner.message.error);
          }

          // The owner is a separate leased module. Disposing its iframe is
          // deliberately not an orderly WasmFS/backend shutdown assertion.
          owner.frame.remove();

          const mutator = await startLeasedModule(
            mutatorPath, kMutator, phase + ' mutator');
          if (mutator.message.result != kReady || mutator.message.error != 0) {
            mutator.frame.remove();
            throw new Error(phase + ' mutator failed: result=' +
                            mutator.message.result + ', errno=' +
                            mutator.message.error);
          }

          await waitForWitness(phase);
          // This disposes the document whose ProxyWorker callback remains
          // pending. No page JavaScript reads or writes OPFS on its behalf.
          mutator.frame.remove();

          const verifier = await startLeasedModule(
            'verifier.html', kVerifier, phase + ' verifier');
          if (verifier.message.result != disposition ||
              verifier.message.error != 0) {
            verifier.frame.remove();
            const expected = disposition == kPreMove ? 'A + tmp-B' :
                                                       'B + no-tmp';
            throw new Error(phase + ' fresh verifier observed result=' +
                            verifier.message.result + ', errno=' +
                            verifier.message.error + ', expected ' + expected);
          }
          verifier.frame.remove();
        }

        (async () => {
          await runInterruptionCase(
            'mutator-before.html', 'before', kPreMove);
          await runInterruptionCase(
            'mutator-after.html', 'after', kPostMove);
          witnessChannel.close();
          reportResultToServer('0');
        })().catch((error) => {
          witnessChannel.close();
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=90)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_dup3_close(self):
    test = test_file('wasmfs/wasmfs_opfs_dup3_close.c')
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD',
                   '-sEXIT_RUNTIME', '-lopfs.js']
    create_file('dup3-holder-pre.js', r'''
      // This runs on the browser main runtime after native atexit handlers.
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-dup3-close',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_DUP3_CLOSE_HOLDER', '--pre-js',
                     'dup3-holder-pre.js', '-o', 'dup3-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-o', 'dup3-contender.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kModuleTimeoutMs = 15000;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder shutdown'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-dup3-close') {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status != 0) {
              pending.reject(new Error(
                'holder did not exit cleanly: status=' + event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        (async () => {
          const holder = await startModule('dup3-holder.html');
          if (holder.message.role != kHolder || holder.message.error != 0) {
            throw new Error('holder failed: role=' + holder.message.role +
                            ', errno=' + holder.message.error);
          }

          // The holder has reported only after dup3 returned, and it keeps its
          // runtime alive until this explicit request after the contender has
          // opened the original victim file.
          const contender = await startModule('dup3-contender.html');
          if (contender.message.role != kContender ||
              contender.message.error != 0) {
            throw new Error('contender could not acquire victim access: role=' +
                            contender.message.role + ', errno=' +
                            contender.message.error);
          }

          // The holder's onExit witness follows its explicit close calls and
          // WasmFS global teardown, so do not remove its frame before it.
          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_dup3_close_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=60)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_close_failure(self):
    test = test_file('wasmfs/wasmfs_opfs_close_failure.c')
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD',
                   '-sEXIT_RUNTIME', '-lopfs.js']
    create_file('close-failure-holder-pre.js', r'''
      // This runs on the browser main runtime after native atexit handlers.
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-close-failure',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_OPFS_CLOSE_FAILURE_HOLDER',
                     '-sWASMFS_OPFS_TEST_CLOSE_FAILURE=1', '--pre-js',
                     'close-failure-holder-pre.js',
                     '-o', 'close-failure-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-o', 'close-failure-contender.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kModuleTimeoutMs = 15000;
        const kTraceType = 'wasmfs-opfs-test-close-failure';
        const pendingModules = new Map();
        const pendingHolderExits = new Map();
        const pendingWitnesses = new Map();
        const witnessWaiters = new Map();
        const witnessChannel = new BroadcastChannel(kTraceType);

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder shutdown'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {resolve, reject, timeout});
          });
        }

        function waitForWitness(phase) {
          if (pendingWitnesses.has(phase)) {
            const witness = pendingWitnesses.get(phase);
            pendingWitnesses.delete(phase);
            return Promise.resolve(witness);
          }
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              witnessWaiters.delete(phase);
              reject(new Error('timed out waiting for ' + phase +
                               ' close-failure witness'));
            }, kModuleTimeoutMs);
            witnessWaiters.set(phase, {resolve, timeout});
          });
        }

        witnessChannel.onmessage = (event) => {
          const witness = event.data;
          if (witness?.type != kTraceType ||
              (witness.phase != 'close-rejected' &&
               witness.phase != 'next-access')) {
            return;
          }
          const waiter = witnessWaiters.get(witness.phase);
          if (waiter) {
            witnessWaiters.delete(witness.phase);
            clearTimeout(waiter.timeout);
            waiter.resolve(witness);
          } else {
            pendingWitnesses.set(witness.phase, witness);
          }
        };

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-close-failure') {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status != 0) {
              pending.reject(new Error(
                'holder did not exit cleanly: status=' + event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        (async () => {
          const holder = await startModule('close-failure-holder.html');
          if (holder.message.role != kHolder || holder.message.error != 0) {
            throw new Error('holder failed: role=' + holder.message.role +
                            ', errno=' + holder.message.error);
          }

          const rejectedClose = await waitForWitness('close-rejected');
          const nextAccess = await waitForWitness('next-access');
          if (!Number.isInteger(rejectedClose.accessID) ||
              !Number.isInteger(nextAccess.accessID) ||
              rejectedClose.accessID <= 0 || nextAccess.accessID <= 0) {
            throw new Error('invalid access-handle trace IDs');
          }
          if (rejectedClose.accessID == nextAccess.accessID) {
            throw new Error('failed access-handle slot was reused: ' +
                            rejectedClose.accessID);
          }

          // The injected failure was before browser close(), so the contender
          // can report success only if it correctly observed writer exclusion.
          const contender = await startModule('close-failure-contender.html');
          if (contender.message.role != kContender ||
              contender.message.error != 0) {
            throw new Error('contender did not observe target writer exclusion: '
                            + 'role=' + contender.message.role + ', errno=' +
                            contender.message.error);
          }

          // The holder closed its unrelated descriptor before reporting. Its
          // onExit witness proves poison-state teardown did not hide an abort.
          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_opfs_close_failure_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
          witnessChannel.close();
          reportResultToServer('0');
        })().catch((error) => {
          witnessChannel.close();
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=60)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_fd_table_full_close(self):
    test = test_file('wasmfs/wasmfs_opfs_fd_table_full_close.c')
    common_args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD',
                   '-sEXIT_RUNTIME', '-lopfs.js']
    create_file('fd-table-holder-pre.js', r'''
      // This runs on the browser main runtime after native atexit handlers.
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-fd-table-full-close',
          },
          window.location.origin);
      };
    ''')
    self.compile_btest(
      test,
      common_args + ['-DWASMFS_FD_TABLE_FULL_HOLDER', '--pre-js',
                     'fd-table-holder-pre.js', '-o', 'fd-table-holder.html'],
      reporting=Reporting.NONE)
    self.compile_btest(
      test,
      common_args + ['-o', 'fd-table-contender.html'],
      reporting=Reporting.NONE)
    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kModuleTimeoutMs = 30000;
        const pendingModules = new Map();
        const pendingHolderExits = new Map();

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder shutdown'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != 'wasmfs-opfs-fd-table-full-close') {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pending = pendingHolderExits.get(event.source);
            if (!pending) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pending.timeout);
            if (event.data.status != 0) {
              pending.reject(new Error(
                'holder did not exit cleanly: status=' + event.data.status));
            } else {
              pending.resolve();
            }
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        (async () => {
          const holder = await startModule('fd-table-holder.html');
          if (holder.message.role != kHolder || holder.message.error != 0) {
            throw new Error('holder failed: role=' + holder.message.role +
                            ', errno=' + holder.message.error);
          }

          const contender = await startModule('fd-table-contender.html');
          if (contender.message.role != kContender ||
              contender.message.error != 0) {
            throw new Error('contender could not acquire victim access: role=' +
                            contender.message.role + ', errno=' +
                            contender.message.error);
          }

          const holderExit = waitForHolderExit(holder.frame);
          holder.frame.contentWindow.Module
            ._wasmfs_fd_table_full_holder_shutdown();
          await holderExit;
          contender.frame.remove();
          holder.frame.remove();
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    ''')
    self.run_browser('a.html', '/report_result?0', timeout=90)

  @no_firefox('no OPFS support yet')
  @no_safari('TODO: Fails with exception:Did not get expected EIO when unlinking file') # Fails in Safari 17.6 (17618.3.11.11.7, 17618) and Safari 26.0.1 (21622.1.22.11.15)
  def test_wasmfs_opfs_errors(self):
    test = test_file('wasmfs/wasmfs_opfs_errors.c')
    postjs = test_file('wasmfs/wasmfs_opfs_errors_post.js')
    args = ['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '--post-js', postjs]
    self.btest(test, cflags=args, expected='0')

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  def test_wasmfs_opfs_get_child_error(self):
    self.btest_exit(
      'wasmfs/wasmfs_opfs_get_child_error.c',
      cflags=['-sWASMFS', '-pthread', '-sPROXY_TO_PTHREAD', '-sEXIT_RUNTIME',
              '-sWASMFS_OPFS_TEST_GET_CHILD_ERROR=1',
              '-sWASMFS_OPFS_TEST_GET_CHILD_PROXY_FAILURE=1',
              '-sWASMFS_OPFS_TEST_GET_CHILD_MALFORMED_RESULT=1'])

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle support yet')
  @no_wasm64()
  def test_wasmfs_opfs_direct_operation_admission_race(self):
    self.compile_btest(
      'wasmfs/wasmfs_opfs_direct_operation_admission_race.c',
      [
        '-sWASMFS',
        '-sFORCE_FILESYSTEM',
        '-pthread',
        '-sPROXY_TO_PTHREAD',
        '-sPTHREAD_POOL_SIZE=4',
        '-sEXIT_RUNTIME',
        '-sALLOW_BLOCKING_ON_MAIN_THREAD=0',
        '-sWASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE=2',
        '-DWASMFS_OPFS_DIRECT_ADMISSION_RACE_MOUNT='
        'wasmfs_direct_admission_%016x' % random.getrandbits(64),
        '-o', 'direct-opfs-admission-race.html',
      ],
      reporting=Reporting.NONE)
    self.run_browser('direct-opfs-admission-race.html', '/report_result?0',
                     timeout=90)

  @no_firefox('no OPFS support yet')
  @no_safari('no SyncAccessHandle or Web Locks support yet')
  @no_wasm64()
  def test_wasmfs_opfs_direct_proxy_completion_failure(self):
    # This checks direct OPFS fail-closed behavior only. It does not exercise
    # the profile-namespace backend or claim physical crash recovery.
    test = 'wasmfs/wasmfs_opfs_direct_proxy_completion_failure.c'
    common_args = [
      '-sWASMFS',
      '-sFORCE_FILESYSTEM',
      '-pthread',
      '-sPROXY_TO_PTHREAD',
      '-sPTHREAD_POOL_SIZE=4',
      '-sEXIT_RUNTIME',
      '-sALLOW_BLOCKING_ON_MAIN_THREAD=0',
      '-lopfs.js',
    ]
    nonce = f'{random.getrandbits(64):016x}'
    cases = [
      ('root', 1),
      ('create-file', 2),
      ('create-directory', 3),
      ('remove', 4),
      ('enumerate', 5),
    ]
    create_file('direct-proxy-holder-pre.js', r'''
      Module['onExit'] = (status) => {
        window.parent.postMessage(
          {
            event: 'holder-exit',
            status,
            type: 'wasmfs-opfs-direct-proxy-completion',
          },
          window.location.origin);
      };
    ''')
    case_literals = []
    for stem, phase in cases:
      profile = f'wasmfs_direct_proxy_{nonce}_{phase}'
      mount = f'direct_proxy_{nonce}_{phase}'
      holder = f'direct-proxy-{stem}-holder.html'
      contender = f'direct-proxy-{stem}-contender.html'
      fixture_args = [
        '-DWASMFS_OPFS_DIRECT_PROXY_COMPLETION_PHASE=' + str(phase),
        '-DWASMFS_OPFS_DIRECT_PROXY_COMPLETION_PROFILE_NAME=' + profile,
        '-DWASMFS_OPFS_DIRECT_PROXY_COMPLETION_MOUNT_NAME=' + mount,
      ]
      self.compile_btest(
        test,
        common_args + fixture_args + [
          '-DWASMFS_OPFS_DIRECT_PROXY_COMPLETION_HOLDER',
          '-sWASMFS_OPFS_TEST_DIRECTORY_PROXY_COMPLETION_FAILURE=' +
          str(phase),
          '--pre-js', 'direct-proxy-holder-pre.js',
          '-o', holder,
        ],
        reporting=Reporting.NONE)
      self.compile_btest(
        test,
        common_args + fixture_args + [
          '-DWASMFS_OPFS_DIRECT_PROXY_COMPLETION_CONTENDER',
          '-o', contender,
        ],
        reporting=Reporting.NONE)
      case_literals.append(
        "{ contender: '%s', holder: '%s', name: '%s' }" %
        (contender, holder, stem))

    self.add_browser_reporting()
    create_file('a.html', r'''
      <!doctype html>
      <meta charset="utf-8">
      <body></body>
      <script src="browser_reporting.js"></script>
      <script>
        const kHolder = 0;
        const kContender = 1;
        const kReady = 0;
        const kBusy = 1;
        const kModuleTimeoutMs = 15000;
        const kLeaseReleaseDeadlineMs = 10000;
        const kLeaseRetryDelayMs = 100;
        const kMessageType = 'wasmfs-opfs-direct-proxy-completion';
        const cases = [__DIRECT_PROXY_CASES__];
        const pendingHolderExits = new Map();
        const pendingModules = new Map();

        function delay(ms) {
          return new Promise((resolve) => setTimeout(resolve, ms));
        }

        function startModule(path) {
          const frame = document.createElement('iframe');
          frame.style.display = 'none';
          document.body.appendChild(frame);
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingModules.delete(frame.contentWindow);
              frame.remove();
              reject(new Error('timed out waiting for ' + path));
            }, kModuleTimeoutMs);
            pendingModules.set(frame.contentWindow, {frame, resolve, timeout});
            frame.src = path;
          });
        }

        function waitForHolderExit(frame) {
          return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
              pendingHolderExits.delete(frame.contentWindow);
              reject(new Error('timed out waiting for holder cleanup'));
            }, kModuleTimeoutMs);
            pendingHolderExits.set(
              frame.contentWindow, {resolve, reject, timeout});
          });
        }

        window.addEventListener('message', (event) => {
          if (event.origin != window.location.origin ||
              event.data?.type != kMessageType) {
            return;
          }
          if (event.data.event == 'holder-exit') {
            const pendingExit = pendingHolderExits.get(event.source);
            if (!pendingExit) {
              return;
            }
            pendingHolderExits.delete(event.source);
            clearTimeout(pendingExit.timeout);
            if (event.data.status != 0) {
              pendingExit.reject(new Error(
                'holder cleanup exited with status ' + event.data.status));
            } else {
              pendingExit.resolve();
            }
            return;
          }
          const pending = pendingModules.get(event.source);
          if (!pending) {
            return;
          }
          pendingModules.delete(event.source);
          clearTimeout(pending.timeout);
          pending.resolve({frame: pending.frame, message: event.data});
        });

        async function waitForHolderContextRelease(testCase) {
          // The failed holder intentionally retains its Web Lock until its
          // document is disposed. Browser-context lock cleanup is asynchronous,
          // so confirm that it has completed before starting the next phase or
          // an unrelated direct-OPFS browser test. This is cleanup only: the
          // live-holder contender above is the no-release assertion.
          const deadline = Date.now() + kLeaseReleaseDeadlineMs;
          while (Date.now() < deadline) {
            const candidate = await startModule(testCase.contender);
            try {
              if (candidate.message.role != kContender) {
                throw new Error(testCase.name +
                                ' cleanup contender reported role ' +
                                candidate.message.role);
              }
              if (candidate.message.result == kReady &&
                  candidate.message.error == 0) {
                return;
              }
              if (candidate.message.result != kBusy ||
                  candidate.message.error == 0) {
                throw new Error(testCase.name +
                                ' cleanup contender failed: result=' +
                                candidate.message.result + ', errno=' +
                                candidate.message.error);
              }
            } finally {
              // A ready contender has already completed its successful
              // terminal drain; a busy contender owns no lease. Either frame
              // may be dropped before the next cleanup poll.
              candidate.frame.remove();
            }
            await delay(kLeaseRetryDelayMs);
          }
          throw new Error(testCase.name +
                          ' holder context did not release its profile lease');
        }

        async function runCase(testCase) {
          let holder;
          let contender;
          try {
            holder = await startModule(testCase.holder);
            if (holder.message.role != kHolder ||
                holder.message.result != kReady || holder.message.error != 0) {
              throw new Error(testCase.name + ' holder did not fail closed: ' +
                              'role=' + holder.message.role + ', result=' +
                              holder.message.result + ', errno=' +
                              holder.message.error);
            }

            // The holder has completed its failed terminal drain but remains
            // alive. EBUSY here is the lease-retention witness; disposing the
            // holder below is only test cleanup, not persistence evidence.
            contender = await startModule(testCase.contender);
            if (contender.message.role != kContender ||
                contender.message.result != kBusy ||
                contender.message.error == 0) {
              throw new Error(testCase.name + ' contender did not observe a ' +
                              'retained lease: role=' + contender.message.role +
                              ', result=' + contender.message.result +
                              ', errno=' + contender.message.error);
            }
            contender.frame.remove();
            contender = undefined;

            const holderExit = waitForHolderExit(holder.frame);
            holder.frame.contentWindow.Module
              ._wasmfs_opfs_direct_proxy_completion_holder_shutdown();
            await holderExit;
            holder.frame.remove();
            holder = undefined;
            await waitForHolderContextRelease(testCase);
          } finally {
            if (contender) {
              contender.frame.remove();
            }
            if (holder) {
              pendingHolderExits.delete(holder.frame.contentWindow);
              holder.frame.remove();
            }
          }
        }

        (async () => {
          for (const testCase of cases) {
            await runCase(testCase);
          }
          reportResultToServer('0');
        })().catch((error) => {
          reportResultToServer('failure: ' + error.message);
        });
      </script>
    '''.replace('__DIRECT_PROXY_CASES__', ', '.join(case_literals)))
    self.run_browser('a.html', '/report_result?0', timeout=120)

  def test_wasmfs_multi_environment(self):
    # Test that WasmFS's Node backend can be enabled conditionally, allowing
    # the same binaries to run on both web and Node.js environments.
    create_file('main.c', r'''
      #include <stdio.h>
      #include <assert.h>
      #include <unistd.h>

      #include <emscripten/emscripten.h>
      #include <emscripten/wasmfs.h>

      EM_JS(bool, is_node, (), { return ENVIRONMENT_IS_NODE; });

      // This is equivalent to building with `-sWASMFS -sNODERAWFS`, except
      // that the Wasm binary can also be used on the web.
      backend_t wasmfs_create_root_dir() {
        return is_node() ? wasmfs_create_node_backend("")
                         : wasmfs_create_memory_backend();
      }

      int main(int argc, char** argv) {
        printf("testing access to /tmp\n");
        int rtn = access("/tmp", F_OK);
        assert(rtn == 0);
        return 0;
      }
    ''')
    self.btest_exit('main.c', cflags=['-sWASMFS', '-sENVIRONMENT=web,node'])

  @no_firefox('no 4GB support yet')
  def test_emmalloc_memgrowth(self):
    if not self.is_4gb():
      self.set_setting('MAXIMUM_MEMORY', '4GB')
    self.btest_exit('emmalloc_memgrowth.cpp', cflags=['-sMALLOC=emmalloc', '-sALLOW_MEMORY_GROWTH=1', '-sABORTING_MALLOC=0', '-sASSERTIONS=2', '-sMINIMAL_RUNTIME=1'])

  @no_firefox('no 4GB support yet')
  @no_2gb('uses MAXIMUM_MEMORY')
  @no_4gb('uses MAXIMUM_MEMORY')
  def test_2gb_fail(self):
    # TODO Convert to an actual browser test when it reaches stable.
    #      For now, keep this in browser as this suite runs serially, which
    #      means we don't compete for memory with anything else (and run it
    #      at the very very end, to reduce the risk of it OOM-killing the
    #      browser).

    # test that growth doesn't go beyond 2GB without the max being set for that,
    # and that we can catch an allocation failure exception for that
    self.cflags += ['-O2', '-sALLOW_MEMORY_GROWTH', '-sMAXIMUM_MEMORY=2GB']
    self.do_run_in_out_file_test('browser/test_2GB_fail.cpp')

  @no_firefox('no 4GB support yet')
  @no_2gb('uses MAXIMUM_MEMORY')
  @no_4gb('uses MAXIMUM_MEMORY')
  def test_4gb_fail(self):
    # TODO Convert to an actual browser test when it reaches stable.
    #      For now, keep this in browser as this suite runs serially, which
    #      means we don't compete for memory with anything else (and run it
    #      at the very very end, to reduce the risk of it OOM-killing the
    #      browser).

    # test that we properly report an allocation error that would overflow over
    # 4GB.
    self.set_setting('MAXIMUM_MEMORY', '4GB')
    self.cflags += ['-O2', '-sALLOW_MEMORY_GROWTH', '-sABORTING_MALLOC=0', '-sASSERTIONS']
    self.do_run_in_out_file_test('browser/test_4GB_fail.cpp')

  # Tests that Emscripten-compiled applications can be run when a slash in the URL query or fragment of the js file
  def test_browser_run_with_slash_in_query_and_hash(self):
    self.compile_btest('browser_test_hello_world.c', ['-o', 'test.html', '-O0'])
    src = utils.read_file('test.html')
    # Slash in query
    create_file('test-query.html', src.replace('test.js', 'test.js?type=pass/fail'))
    self.run_browser('test-query.html', '/report_result?0')
    # Slash in fragment
    create_file('test-hash.html', src.replace('test.js', 'test.js#pass/fail'))
    self.run_browser('test-hash.html', '/report_result?0')
    # Slash in query and fragment
    create_file('test-query-hash.html', src.replace('test.js', 'test.js?type=pass/fail#pass/fail'))
    self.run_browser('test-query-hash.html', '/report_result?0')

  @disabled("only run this manually, to test for race conditions")
  @parameterized({
    'normal': ([],),
    'assertions': (['-sASSERTIONS'],),
  })
  def test_manual_pthread_proxy_hammer(self, args):
    # the specific symptom of the hang that was fixed is that the test hangs
    # at some point, using 0% CPU. often that occurred in 0-200 iterations, but
    # you may want to adjust "ITERATIONS".
    self.btest_exit('pthread/test_pthread_proxy_hammer.cpp',
                    cflags=['-pthread', '-O2', '-sPROXY_TO_PTHREAD',
                               '-DITERATIONS=1024', '-g1'] + args,
                    timeout=10000)

  def test_assert_failure(self):
    self.btest('test_assert_failure.c', 'abort:Assertion failed: false && "this is a test"')

  @no_safari('TODO: Fails with report_result?exception:rejected!') # Fails in Safari 17.6 (17618.3.11.11.7, 17618), Safari 26.0.1 (21622.1.22.11.15)
  def test_pthread_unhandledrejection(self):
    # Check that an unhandled promise rejection is propagated to the main thread
    # as an error. This test is failing if it hangs!
    self.btest('pthread/test_pthread_unhandledrejection.c',
               cflags=['-pthread', '-sPROXY_TO_PTHREAD', '--post-js',
                          test_file('pthread/test_pthread_unhandledrejection.post.js')],
               # Firefox and Chrome report this slightly differently
               expected=['exception:Uncaught rejected!', 'exception:uncaught exception: rejected!'])

  def test_pthread_key_recreation(self):
    self.btest_exit('pthread/test_pthread_key_recreation.c', cflags=['-pthread', '-sPTHREAD_POOL_SIZE=1'])

  def test_full_js_library_strict(self):
    self.btest_exit('hello_world.c', cflags=['-sINCLUDE_FULL_LIBRARY', '-sSTRICT_JS'])

  # Tests the AudioWorklet demo
  @parameterized({
    '': ([],),
    'with_fs': (['--preload-file', test_file('hello_world.c') + '@/'],),
    'closure': (['--closure', '1', '-Oz'],),
    'asyncify': (['-sASYNCIFY'],),
    'pthreads': (['-pthread', '-sPTHREAD_POOL_SIZE=2'],),
    'pthreads_and_closure': (['-pthread', '--closure', '1', '-Oz'],),
    'minimal_runtime': (['-sMINIMAL_RUNTIME'],),
    'minimal_runtime_pthreads_and_closure': (['-sMINIMAL_RUNTIME', '-pthread', '--closure', '1', '-Oz'],),
    'pthreads_es6': (['-pthread', '-sPTHREAD_POOL_SIZE=2', '-sEXPORT_ES6'],),
    'es6': (['-sEXPORT_ES6'],),
    'strict': (['-sSTRICT'],),
    'audio_params_disabled': (['-sAUDIO_WORKLET_SUPPORT_AUDIO_PARAMS=0'],),
  })
  @requires_sound_hardware
  @requires_es6_workers
  @requires_shared_array_buffer
  def test_audio_worklet(self, args):
    self.btest_exit('webaudio/audioworklet.c', cflags=['-sAUDIO_WORKLET', '-sWASM_WORKERS', '-DTEST_AND_EXIT'] + args)

  # Tests that audioworklets and workers can be used at the same time
  # Note: doesn't need audio hardware (and has no AW code that tests 2GB or wasm64)
  @requires_shared_array_buffer
  def test_audio_worklet_worker(self):
    self.btest_exit('webaudio/audioworklet_worker.c', cflags=['-sAUDIO_WORKLET', '-sWASM_WORKERS'])

  # Tests that posting functions between the main thread and the audioworklet thread works
  @parameterized({
    '': ([],),
    'closure': (['--closure', '1', '-Oz'],),
  })
  # Note: doesn't need audio hardware (and has no AW code that tests 2GB or wasm64)
  @requires_shared_array_buffer
  def test_audio_worklet_post_function(self, args):
    self.btest_exit('webaudio/audioworklet_post_function.c', cflags=['-sAUDIO_WORKLET', '-sWASM_WORKERS'] + args)

  @parameterized({
    '': ([],),
    'closure': (['--closure', '1', '-Oz'],),
  })
  @requires_sound_hardware
  @requires_shared_array_buffer
  def test_audio_worklet_modularize(self, args):
    self.btest_exit('webaudio/audioworklet.c', cflags=['-sAUDIO_WORKLET', '-sWASM_WORKERS', '-sMODULARIZE=1', '-DTEST_AND_EXIT'] + args)

  # Tests an AudioWorklet with multiple stereo inputs mixing in the processor
  # via a varying parameter to a single stereo output (touching all of the API
  # copying from structs)
  @parameterized({
    '': ([],),
    'minimal_with_closure': (['-sMINIMAL_RUNTIME', '--closure=1', '-Oz'],),
  })
  @requires_sound_hardware
  @requires_shared_array_buffer
  def test_audio_worklet_params_mixing(self, args):
    os.mkdir('audio_files')
    copy_asset('webaudio/audio_files/emscripten-beat.mp3', 'audio_files/')
    copy_asset('webaudio/audio_files/emscripten-bass.mp3', 'audio_files/')
    self.btest_exit('webaudio/audioworklet_params_mixing.c', cflags=['-sAUDIO_WORKLET', '-sWASM_WORKERS', '-DTEST_AND_EXIT'] + args)

  # Tests AudioWorklet with emscripten_lock_busyspin_wait_acquire() and friends
  @requires_sound_hardware
  @requires_shared_array_buffer
  @also_with_minimal_runtime
  def test_audio_worklet_emscripten_locks(self):
    self.btest_exit('webaudio/audioworklet_emscripten_locks.c', cflags=['-sAUDIO_WORKLET', '-sWASM_WORKERS', '-pthread'])

  def test_audio_worklet_direct(self):
    self.add_browser_reporting()
    self.emcc('hello_world.c', ['-o', 'hello_world.mjs', '-sEXPORT_ES6', '-sSINGLE_FILE', '-sENVIRONMENT=worklet'])
    create_file('worklet.mjs', '''
      import Module from "./hello_world.mjs"
      console.log("in worklet");
      class MyProcessor extends AudioWorkletProcessor {
        constructor() {
          super();
          Module().then(() => {
            console.log("done Module constructor");
            this.port.postMessage("ready");
          });
        }
        process(inputs, outputs, parameters) {
          return true;
        }
      }
      registerProcessor('my-processor', MyProcessor);
      console.log("done register");
    ''')
    create_file('test.html', '''
      <script src="browser_reporting.js"></script>
      <script>
        async function createContext() {
          const context = new window.AudioContext();
          await context.audioWorklet.addModule('worklet.mjs');
          const node = new AudioWorkletNode(context, 'my-processor');
          node.port.onmessage = (event) => {
            console.log(event);
            reportResultToServer(event.data);
          }
        }
        createContext();
      </script>
    ''')
    self.run_browser('test.html', '/report_result?ready')

  # Verifies setting audio context sample rate, and that emscripten_audio_context_sample_rate() works.
  @requires_sound_hardware
  @also_with_minimal_runtime
  def test_web_audio_context_sample_rate(self):
    self.btest_exit('webaudio/audio_context_sample_rate.c', cflags=['-lwebaudio.js'])

  def test_error_reporting(self):
    # Test catching/reporting Error objects
    create_file('post.js', 'throw new Error("oops");')
    self.btest('hello_world.c', cflags=['--post-js=post.js'], expected='exception:oops')

    # Test catching/reporting non-Error objects
    create_file('post.js', 'throw "foo";')
    self.btest('hello_world.c', cflags=['--post-js=post.js'], expected='exception:foo')

  @also_with_threads
  @also_with_wasm2js
  @parameterized({
    '': ([],),
    'es6': (['-sEXPORT_ES6', '-pthread', '-sPTHREAD_POOL_SIZE=1'],),
  })
  @requires_dev_dependency('webpack')
  def test_webpack(self, args):
    if '-sEXPORT_ES6' in args:
      copytree(test_file('webpack_es6'), '.')
      outfile = 'src/hello.mjs'
    else:
      copytree(test_file('webpack'), '.')
      outfile = 'src/hello.js'
    self.compile_btest('hello_world.c', ['-sEXIT_RUNTIME', '-sMODULARIZE', '-sENVIRONMENT=web', '-o', outfile] + args)
    self.run_process(shared.get_npm_cmd('webpack') + ['--mode=development', '--no-devtool'])
    if not self.is_wasm2js():
      # Webpack doesn't bundle the wasm file by default so we need to copy it
      # TODO(sbc): Look into plugins that do bundling.
      shutil.copy('src/hello.wasm', 'dist/')
    self.run_browser('dist/index.html', '/report_result?exit:0')

  @also_with_threads
  @requires_dev_dependency('vite')
  @parameterized({
    '': ([],),
    'minimal': (['-sMINIMAL_RUNTIME', '-sMINIMAL_RUNTIME_STREAMING_WASM_INSTANTIATION'],),
  })
  def test_vite(self, args):
    copytree(test_file('vite'), '.')
    self.compile_btest('hello_world.c', ['-sEXIT_RUNTIME', '-sENVIRONMENT=web', '-o', 'hello.mjs'] + args)
    self.run_process(shared.get_npm_cmd('vite') + ['build'])
    self.run_browser('dist/index.html', '/report_result?exit:0')

  @also_with_threads
  @requires_dev_dependency('rollup')
  def test_rollup(self):
    copytree(test_file('rollup'), '.')
    self.compile_btest('hello_world.c', ['-sEXIT_RUNTIME', '-sENVIRONMENT=web', '-o', 'hello.mjs'])
    self.run_process(shared.get_npm_cmd('rollup') + ['--config'])
    # Rollup doesn't bundle the wasm file by default so we need to copy it
    # TODO(sbc): Look into plugins that do bundling.
    shutil.copy('hello.wasm', 'dist/')
    self.run_browser('index.html', '/report_result?exit:0')

  # Use different ports for each parameterized version so they can be run in
  # parallel and not conflict.
  @parameterized({
    '': ([], 9998),
    'es6': (['-sEXPORT_ES6', '--extern-post-js', test_file('modularize_post_js.js')], 9999),
  })
  @requires_shared_array_buffer
  def test_cross_origin(self, args, port):
    if '-sEXPORT_ES6' in args and browser_should_skip_feature('EMTEST_LACKS_ES6_WORKERS', Feature.WORKER_ES6_MODULES):
      self.skipTest('This test requires a browser with ES6 Module Workers support')
    # Verifies that the emscripten-generated JS and Wasm can be hosted on a different origin.
    # This test creates a second HTTP server running on a different port that serves files from `subdir`.
    # The main html is served from the normal port 8888 server while the JS and Wasm are hosted
    # on the port specified above.
    os.mkdir('subdir')
    create_file('subdir/foo.txt', 'hello')
    self.compile_btest('hello_world.c', ['-o', 'subdir/hello.js', '-sRUNTIME_DEBUG', '-sCROSS_ORIGIN', '-sPROXY_TO_PTHREAD', '-pthread', '-sEXIT_RUNTIME'] + args)

    class MyRequestHandler(SimpleHTTPRequestHandler):
      def __init__(self, *args, **kwargs):
        super().__init__(*args, directory='subdir', **kwargs)

      # Add COOP, COEP, CORP, and no-caching headers
      def end_headers(self):
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')

        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate, private, max-age=0')
        self.send_header('Expires', '0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Vary', '*') # Safari insists on caching if this header is not present in addition to the above

        return SimpleHTTPRequestHandler.end_headers(self)

    if '-sEXPORT_ES6' in args:
      create_file('test.html', f'<script src="http://localhost:{port}/hello.js" type="module"></script>')
    else:
      create_file('test.html', f'<script src="http://localhost:{port}/hello.js"></script>')

    server = HttpServerThread(ThreadingHTTPServer(('localhost', port), MyRequestHandler))
    server.start()
    try:
      self.run_browser('test.html', '/report_result?exit:0')
    finally:
      server.stop()
      server.join()

  # Tests encoding of all byte pairs for binary encoding in SINGLE_FILE mode.
  @parameterized({
    '': ('',),
    'strict': ('"use strict";',),
  })
  def test_binary_encode(self, extra):
    # Encode values 0 .. 65535 into test data
    test_data = bytearray(struct.pack('<' + 'H' * 65536, *range(65536)))
    write_binary('data.tmp', test_data)
    binary_encoded = binary_encode('data.tmp')

    write_file('test.html', '''<!DOCTYPE html><html><head><meta charset="utf-8"></head><body><script>
''' + extra + '\n' + read_file(path_from_root('src/binaryDecode.js')) + '''
var src = ''' + binary_encoded + ''';
var u16 = new Uint16Array(binaryDecode(src).buffer);
for(var i = 0; i < 65536; ++i)
  if (u16[i] != i) throw i;
console.log('OK');
fetch('report_result?0');
</script></body></html>
''')

    self.run_browser('test.html', '/report_result?0')

  @parameterized({
    '': ([],),
    'modularize': (['-sMODULARIZE'],),
  })
  def test_shell_minimal(self, args):
    self.btest_exit('browser_test_hello_world.c', cflags=['--shell-file', path_from_root('html/shell_minimal.html')] + args)


class emrun(RunnerCore):
  def test_emrun_info(self):
    if not has_browser():
      self.skipTest('need a browser')
    result = self.run_process([EMRUN, '--system-info', '--browser_info'], stdout=PIPE).stdout
    assert 'CPU' in result
    assert 'Browser' in result
    assert 'Traceback' not in result

    result = self.run_process([EMRUN, '--list-browsers'], stdout=PIPE).stdout
    assert 'Traceback' not in result

  def test_no_browser(self):
    # Test --no-browser mode where we have to take care of launching the browser ourselves
    # and then killing emrun when we are done.
    if not has_browser():
      self.skipTest('need a browser')

    self.run_process([EMCC, test_file('test_emrun.c'), '--emrun', '-o', 'hello_world.html'])
    proc = subprocess.Popen([EMRUN, '--no-browser', '.', '--port=3333'], stdout=PIPE)
    try:
      if get_browser():
        print('Starting browser')
        browser_cmd = shlex.split(get_browser())
        browser = subprocess.Popen(browser_cmd + ['http://localhost:3333/hello_world.html'])
        try:
          while True:
            stdout = proc.stdout.read()
            if b'Dumping out file' in stdout:
              break
        finally:
          print('Terminating browser')
          browser.terminate()
          browser.wait()
    finally:
      print('Terminating emrun server')
      proc.terminate()
      proc.wait()

  def test_program_arg_separator(self):
    # Verify that trying to pass argument to the page without the `--` separator will
    # generate an actionable error message
    err = self.expect_fail([EMRUN, '--foo'])
    self.assertContained('error: unrecognized arguments: --foo', err)
    self.assertContained('remember to add `--` between arguments', err)

  def test_emrun(self):
    self.emcc('test_emrun.c', ['--emrun', '-o', 'test_emrun.html'])
    if not has_browser():
      self.skipTest('need a browser')

    # We cannot run emrun from the temp directory the suite will clean up afterwards, since the
    # browser that is launched will have that directory as startup directory, and the browser will
    # not close as part of the test, pinning down the cwd on Windows and it wouldn't be possible to
    # delete it. Therefore switch away from that directory before launching.
    os.chdir(path_from_root())

    args_base = [EMRUN, '--timeout', '30', '--safe_firefox_profile',
                 '--kill-exit', '--port', '6939', '--verbose',
                 '--log-stdout', self.in_dir('stdout.txt'),
                 '--log-stderr', self.in_dir('stderr.txt')]

    if get_browser() is not None:
      # If EMTEST_BROWSER carried command line arguments to pass to the browser,
      # (e.g. "firefox -profile /path/to/foo") those can't be passed via emrun,
      # so strip them out.
      browser_cmd = shlex.split(get_browser())
      browser_path = browser_cmd[0]
      args_base += ['--browser', browser_path]
      if len(browser_cmd) > 1:
        browser_args = browser_cmd[1:]
        if 'firefox' in browser_path and ('-profile' in browser_args or '--profile' in browser_args):
          # emrun uses its own -profile, strip it out
          parser = argparse.ArgumentParser(add_help=False) # otherwise it throws with -headless
          parser.add_argument('-profile')
          parser.add_argument('--profile')
          browser_args = parser.parse_known_args(browser_args)[1]
        if browser_args:
          args_base += ['--browser_args', ' ' + ' '.join(browser_args)]

    for args in [
        [],
        ['--port', '0'],
        ['--private_browsing', '--port', '6941'],
        ['--dump_out_directory', 'other dir/multiple', '--port', '6942'],
        ['--dump_out_directory=foo_bar', '--port', '6942'],
    ]:
      args = args_base + args + [self.in_dir('test_emrun.html'), '--', '1', '2', '--3', 'escaped space', 'with_underscore']
      print(shlex.join(args))
      proc = self.run_process(args, check=False)
      self.assertEqual(proc.returncode, 100)
      dump_dir = 'dump_out'
      if '--dump_out_directory' in args:
        dump_dir = 'other dir/multiple'
      elif '--dump_out_directory=foo_bar' in args:
        dump_dir = 'foo_bar'
      self.assertExists(self.in_dir(f'{dump_dir}/test.dat'))
      self.assertExists(self.in_dir(f'{dump_dir}/heap.dat'))
      self.assertExists(self.in_dir(f'{dump_dir}/nested/with space.dat'))
      stdout = read_file(self.in_dir('stdout.txt'))
      stderr = read_file(self.in_dir('stderr.txt'))
      self.assertContained('argc: 6', stdout)
      self.assertContained('argv[3]: --3', stdout)
      self.assertContained('argv[4]: escaped space', stdout)
      self.assertContained('argv[5]: with_underscore', stdout)
      self.assertContained('Hello, world!', stdout)
      self.assertContained('Testing ASCII characters: !"$%&\'()*+,-./:;<=>?@[\\]^_`{|}~', stdout)
      self.assertContained('Testing char sequences: %20%21 &auml;', stdout)
      self.assertContained('hello, error stream!', stderr)


class browser64(browser):
  def setUp(self):
    super().setUp()
    self.set_setting('MEMORY64')
    self.require_wasm64()


class browser64_4gb(browser):
  def setUp(self):
    super().setUp()
    self.set_setting('MEMORY64')
    self.set_setting('INITIAL_MEMORY', '4200mb')
    self.set_setting('GLOBAL_BASE', '4gb')
    # Without this we get a warning about GLOBAL_BASE being ignored when used with SIDE_MODULE
    self.cflags.append('-Wno-unused-command-line-argument')
    self.require_wasm64()


class browser64_2gb(browser):
  def setUp(self):
    super().setUp()
    self.set_setting('MEMORY64')
    self.set_setting('INITIAL_MEMORY', '2200mb')
    self.set_setting('GLOBAL_BASE', '2gb')
    # Without this we get a warning about GLOBAL_BASE being ignored when used with SIDE_MODULE
    self.cflags.append('-Wno-unused-command-line-argument')
    self.require_wasm64()


class browser_2gb(browser):
  def setUp(self):
    super().setUp()
    self.set_setting('INITIAL_MEMORY', '2200mb')
    self.set_setting('GLOBAL_BASE', '2gb')
    # Without this we get a warning about GLOBAL_BASE being ignored when used with SIDE_MODULE
    self.cflags.append('-Wno-unused-command-line-argument')
