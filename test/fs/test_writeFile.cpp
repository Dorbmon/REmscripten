// https://github.com/emscripten-core/emscripten/issues/2334

#include <fstream>
#include <iostream>
#include <string>
#include <errno.h>

#include <emscripten/emscripten.h>

int main() {
  EM_ASM({
    const buf = Uint8Array.from('c=3\nd=4\ne=5', x => x.charCodeAt(0));
    assert(FS.writeFile("testfile", "a=1\nb=2\n") === undefined);
    assert(FS.writeFile("testfile", buf.subarray(4, 7) /* d=4 */,
                        { flags: "a" }) === undefined);

    // FS.writeFile defaults to "w", so the second default write replaces
    // rather than appends to the first one. Explicit "a" remains append.
    assert(FS.writeFile("replace-test", "before") === undefined);
    assert(FS.writeFile("replace-test", "after") === undefined);
    assert(FS.readFile("replace-test", { encoding: "utf8" }) === "after");
    assert(FS.writeFile("replace-test", "!", { flags: "a" }) === undefined);
    assert(FS.readFile("replace-test", { encoding: "utf8" }) === "after!");

    assert(FS.writeFile("readonly-test", "before") === undefined);
    var readonlyError;
    try {
      FS.writeFile("readonly-test", "after", { flags: "r" });
    } catch (err) {
      readonlyError = err;
    }
    assert(readonlyError.name === "ErrnoError" && readonlyError.errno === $1);
    assert(FS.readFile("readonly-test", { encoding: "utf8" }) === "before");

    assert(FS.writeFile("mode-test", "m", { mode: 0o600 }) === undefined);
    assert((FS.stat("mode-test").mode & 0o777) === 0o600);

    FS.mkdir("writefile-directory");
    var ex;
    try {
      FS.writeFile("writefile-directory", "not a file");
    } catch (err) {
      ex = err;
    }
    assert(ex.name === "ErrnoError" && ex.errno === $0);
    FS.rmdir("writefile-directory");
  }, EISDIR, EBADF);

  std::ifstream file("testfile");

  while (!file.eof() && !file.fail()) {
    std::string line;
    getline(file, line);
    std::string key;
    std::string val;

    std::cout << "read " << line << std::endl;

    size_t equalsPos = 1;

    size_t notSpace = line.find_first_not_of(" \t", equalsPos);

    if (notSpace != std::string::npos && notSpace != equalsPos) {
      line.erase(std::remove_if(line.begin(), line.begin() + notSpace, isspace), line.end());

      equalsPos = line.find('=');
    }

    if (equalsPos == std::string::npos) {
        continue;
    }

    key = line.substr(0, equalsPos);
    val = line.substr(equalsPos + 1);

    std::cout << "parsed " << key << "=" << val << std::endl;
  }

  return 0;
}
