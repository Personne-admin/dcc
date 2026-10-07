#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="dcc-no-simd-") as temporary:
    root = Path(temporary)
    source = root / "test.dc"
    for backend in ("custom", "llvm"):
        for opt in ("O0", "O2"):
            for body in ("@nomangle public f64 f(f64 a, f64 b) { return a + b; }",
                         "@nomangle public i32 f(f64 a) { return a as i32; }"):
                if backend == "llvm" and "as i32" in body:
                    continue
                source.write_text("module test;\n" + body)
                result = subprocess.run(["timeout", "60", sys.argv[1], "-fbackend", backend, "-" + opt,
                                         "-fno-simd", "-c", "-o", str(root / "test.o"), str(source)],
                                        capture_output=True, text=True, timeout=65)
                if backend == "custom":
                    assert result.returncode != 0, (backend, opt, result)
                assert "SIMD" in result.stderr or "SSE" in result.stderr, result.stderr
            source.write_text("module test;\n@nomangle public u64 f(u64 a) { return a + 1; }")
            subprocess.run(["timeout", "60", sys.argv[1], "-fbackend", backend, "-" + opt, "-fno-simd",
                            "-c", "-o", str(root / "test.o"), str(source)], check=True, timeout=65)
            dis = subprocess.check_output(["objdump", "-d", str(root / "test.o")], text=True)
            assert "xmm" not in dis and "ymm" not in dis and "zmm" not in dis, dis
print("  RESULT  no-simd diagnostics and integer code passed on both backends at O0/O2")
