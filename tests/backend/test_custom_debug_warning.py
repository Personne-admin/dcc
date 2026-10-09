from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
warning = "debug information is not supported by the custom backend"
with tempfile.TemporaryDirectory(prefix="dcc-custom-debug-") as directory:
    source = Path(directory) / "test.dc"
    source.write_text("module m; @nomangle public i32 f() { return 0; }\n")
    for backend, flags, count in [("custom", ["-g"], 1), ("custom", ["-g", "-g0"], 0), ("custom", [], 0), ("llvm", ["-g"], 0)]:
        result = subprocess.run(["timeout", "60", str(compiler), "-fbackend", backend, *flags, "-c", "-o", str(Path(directory) / "test.o"), str(source)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert result.stderr.count(warning) == count, result.stderr
print("  PASS    4/4 custom debug warning checks")
