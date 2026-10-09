from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
top = Path(__file__).resolve().parents[2]
source_text = """module panic_test;
import std::result;
import std::debug;
import core::source_location;
using std::result::Result;
@nomangle extern void finish(i32 code);
void handler([]const u8 message, core::source_location::SourceLocation sl) {
    if sl.line != EXPECTED { finish(1); }
    if sl.file.len < 8 { finish(2); }
    if sl.file[sl.file.len - 8] != 112 || sl.file[sl.file.len - 7] != 97 || sl.file[sl.file.len - 6] != 110 || sl.file[sl.file.len - 5] != 105 || sl.file[sl.file.len - 4] != 99 || sl.file[sl.file.len - 3] != 46 || sl.file[sl.file.len - 2] != 100 || sl.file[sl.file.len - 1] != 99 { finish(3); }
    if message.len == 0 { finish(4); }
    finish(0);
}
@nomangle public i32 dcc_main() {
    std::debug::set_panic_handler(handler);
    Result(i32, i32) r = Result(i32, i32)::Err(7);
    r.unwrap();
    return 5;
}
"""
source_text = source_text.replace("EXPECTED", str(next(i for i, line in enumerate(source_text.splitlines(), 1) if "r.unwrap();" in line)))
with tempfile.TemporaryDirectory(prefix="dcc-result-location-") as directory:
    root = Path(directory)
    source = root / "panic.dc"
    source.write_text(source_text)
    harness = root / "harness.c"
    harness.write_text("#include <stdlib.h>\nextern int dcc_main(void);\nvoid finish(int code) { exit(code); }\nint main(void) { return dcc_main(); }\n")
    for backend in ("llvm", "custom"):
        for level in ("-O0", "-O2"):
            obj = root / "panic.o"
            result = subprocess.run(["timeout", "60", str(compiler), "-flibdcext", "linux", "-I", str(top / "libdcext"), "-fbackend", backend, level, "-c", str(source), "-o", str(obj)], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            executable = root / "panic"
            subprocess.run(["clang", "-no-pie", str(harness), str(obj), str(compiler.parent.parent / f"lib/libdcext-linux-{backend}.a"), "-lpthread", "-o", str(executable)], check=True, capture_output=True)
            result = subprocess.run(["timeout", "60", str(executable)])
            assert result.returncode == 0, (backend, level, result.returncode)
print("  PASS    4/4 result panic call-site checks")
