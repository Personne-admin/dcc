from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
source_text = """module symbol_test;
import core::source_location;
using core::source_location::{FunctionNameStyle, function_name};
@nomangle extern void capture([]const char name);
@nomangle extern i32 text_abi([]const char text);
@nomangle extern i32 byte_abi([]const u8 bytes);
@runtime []const char ordinary(i32 value) { return function_name(FunctionNameStyle::Mangled); }
@runtime []const char instantiated(T)(T value) { return function_name(FunctionNameStyle::Mangled); }
@nomangle public i32 dcc_main() {
    if text_abi("ASCII") != 0 { return 1; }
    u8[3] binary = {0, 128, 255};
    if byte_abi(binary) != 0 { return 2; }
    capture(ordinary(7));
    capture(instantiated(7));
    capture(ordinary(9));
    capture(instantiated(9));
    return 0;
}
"""
harness_text = r"""#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern int dcc_main(void);
void capture(const char *name, size_t len) { fwrite(name, 1, len, stdout); putchar('\n'); }
int text_abi(const char *text, size_t len) { return len != 5 || memcmp(text, "ASCII", 5); }
int byte_abi(const uint8_t *bytes, size_t len) { return len != 3 || bytes[0] != 0 || bytes[1] != 128 || bytes[2] != 255; }
int main(void) { return dcc_main(); }
"""
with tempfile.TemporaryDirectory(prefix="dcc-function-names-") as directory:
    root = Path(directory)
    source = root / "main.dc"
    harness = root / "harness.c"
    source.write_text(source_text)
    harness.write_text(harness_text)
    for backend in ("llvm", "custom"):
        for level in ("-O0", "-O2"):
            obj = root / "main.o"
            subprocess.run([str(compiler), "-fbackend", backend, level, "-c", str(source), "-o", str(obj)], check=True, capture_output=True, timeout=60)
            symbols = subprocess.run(["nm", "--defined-only", str(obj)], check=True, capture_output=True, text=True).stdout
            readonly = subprocess.run(["readelf", "-p", ".rodata", str(obj)], check=True, capture_output=True, text=True).stdout
            executable = root / "program"
            subprocess.run(["clang", "-no-pie", str(harness), str(obj), "-o", str(executable)], check=True, capture_output=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=60)
            names = result.stdout.splitlines()
            assert len(names) == 4 and names[:2] == names[2:], (backend, level, names)
            for name in names[:2]:
                assert name in symbols, (backend, level, name, symbols)
                assert readonly.count(name) == 1, (backend, level, name, readonly)
print("  PASS    4/4 function symbols, string storage, and text/byte C ABI checks")
