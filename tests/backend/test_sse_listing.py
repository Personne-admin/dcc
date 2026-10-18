from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
checks = 0
with tempfile.TemporaryDirectory(prefix="dcc-sse-listing-") as directory:
    root = Path(directory)
    for bits, ctype in ((32, "float"), (64, "double")):
        for operator, expected in (("+", "14"), ("-", "6"), ("*", "40"), ("/", "2.5")):
            for level in ("-O0", "-O2"):
                source = root / "case.dc"
                source.write_text(f"module test; @nomangle public f{bits} calculate(f{bits} a, f{bits} b) {{ return b {operator} a; }}\n")
                caller = root / "caller.c"
                caller.write_text(f"extern {ctype} calculate({ctype}, {ctype}); int main(void) {{ return calculate(4, 10) != {expected}; }}\n")
                direct, listing, assembled = [root / name for name in ("direct.o", "listing.s", "assembled.o")]
                for flag, output in (("-c", direct), ("-S", listing)):
                    subprocess.run([str(compiler), "-target", "x86_64-elf", "-fbackend", "custom", level,
                                    flag, "-o", str(output), str(source)], check=True, capture_output=True, timeout=60)
                subprocess.run(["nasm", "-f", "elf64", "-o", str(assembled), str(listing)], check=True, capture_output=True, timeout=60)
                for obj in (direct, assembled):
                    executable = root / "run"
                    subprocess.run(["clang", str(caller), str(obj), "-o", str(executable)], check=True, capture_output=True, timeout=60)
                    result = subprocess.run([str(executable)], timeout=5)
                    assert result.returncode == 0, (bits, operator, level, obj.name, listing.read_text())
                    checks += 1
print(f"  PASS    {checks}/{checks} scalar SSE direct/listing execution checks")
