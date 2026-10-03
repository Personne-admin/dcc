#!/usr/bin/env python3
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = """module main;
@noinline @nomangle public i32 first(i32 n) { return n + 1; }
@noinline @nomangle public i32 second(i32 n) { return first(n) * 2; }
@nomangle public i32 dcc_main() { return second(3); }
"""


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main():
    with tempfile.TemporaryDirectory(prefix="dcc-unwind-") as directory:
        work = Path(directory)
        source = work / "main.dc"
        source.write_text(SOURCE)
        for optimization in ("-O0", "-O2"):
            for target in ("x86_64-elf", "x86_64-coff"):
                obj = work / (target + optimization + ".o")
                run("timeout", "60", str(ROOT / "build/bin/dcc"), "-target", target,
                    "-fbackend", "llvm", optimization, "-c", "-o", str(obj), str(source))
                sections = run("llvm-readobj", "--sections", str(obj))
                unwind = run("llvm-readobj", "--unwind", str(obj))
                if target.endswith("elf"):
                    assert "Name: .eh_frame" in sections
                    assert len(re.findall(r"FDE length=", unwind)) == 3, unwind
                else:
                    assert "Name: .pdata" in sections and "Name: .xdata" in sections
                    assert len(re.findall(r"RuntimeFunction \{", unwind)) == 3, unwind
                    for name in ("first", "second", "dcc_main"):
                        assert "StartAddress: " + name in unwind, unwind
                print(f"{target} {optimization}: 3 unwind entries")


if __name__ == "__main__":
    main()
