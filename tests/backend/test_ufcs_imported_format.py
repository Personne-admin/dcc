from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
fixture = Path(__file__).resolve().parents[1] / "cases/em64t/ufcs-imported-format-context-exec.dcc-test"
with tempfile.TemporaryDirectory(prefix="dcc-ufcs-format-") as directory:
    root = Path(directory)
    for name, body in re.findall(r"=== FILE: ([^ ]+) ===\n(.*?)(?=\n===|\Z)", fixture.read_text(), re.S):
        (root / name).write_text(body)
    harness = root / "harness.c"
    harness.write_text("extern int dcc_main(void); int main(void) { return dcc_main(); }\n")
    for backend in ("llvm", "custom"):
        for level in ("-O0", "-O2"):
            objects = []
            for name in ("main", "extension"):
                obj = root / (name + ".o")
                result = subprocess.run([str(compiler), "-fbackend", backend, level, "-I", str(root), "-c", str(root / (name + ".dc")), "-o", str(obj)], capture_output=True, text=True, timeout=60)
                assert result.returncode == 0, result.stderr
                objects.append(str(obj))
            executable = root / "program"
            subprocess.run(["clang", "-no-pie", str(harness), *objects, "-o", str(executable)], check=True, capture_output=True, timeout=60)
            subprocess.run([str(executable)], check=True, capture_output=True, timeout=60)
print("  PASS    4/4 imported formatting overload runtime checks")
