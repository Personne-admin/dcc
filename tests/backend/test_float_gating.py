#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile
from test_memory import run

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="dcc-float-gating-") as temporary:
    scratch = Path(temporary)
    for backend in ("llvm", "custom"):
        for opt in ("O0", "O2"):
            flags = [sys.argv[1], "-flibdcext", "freestanding", "-fbackend", backend, "-" + opt,
                     "-fno-simd", "-fno-x87", "-fno-red-zone", "-fpic", "-I", root / "libdcext"]
            for name in ("math", "parse", "fmt", "json", "toml"):
                run([*flags, "-c", root / ("libdcext/std/" + name + ".dc"), "-o", scratch / (name + ".o")])
            run([*flags, "-c", Path(__file__).with_name("freestanding_modules.dc"), "-o", scratch / "combined.o"])
            source = scratch / "misuse.dc"
            source.write_text("module misuse;\nimport std::math;\npublic f64 misuse(f64 value) { return std::math::fabs(value); }\n")
            result = subprocess.run(["timeout", "60", *map(str, flags), "-c", str(source), "-o", str(scratch / "misuse.o")],
                                    capture_output=True, text=True, timeout=65)
            assert result.returncode != 0, result
            assert "unknown path `std::math::fabs`" in result.stderr, result.stderr
print("  RESULT  no-float modules, integer API imports and gated lookup passed on both backends at O0/O2")
