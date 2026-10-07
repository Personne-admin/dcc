#!/usr/bin/env python3
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

from test_memory import run

with tempfile.TemporaryDirectory(prefix="dcc-missing-library-") as temporary:
    root = Path(temporary)
    dcc = Path(sys.argv[1]).resolve()
    (root / "bin").mkdir()
    fake = root / "bin/dcc"
    try:
        os.link(dcc, fake)
    except OSError:
        shutil.copy2(dcc, fake)
    (root / "include").symlink_to(dcc.parent.parent / "include", target_is_directory=True)
    source = root / "start.dc"
    source.write_text("module start;\n@nomangle public void _start() {}\n")
    for backend in ("llvm", "custom"):
        obj = root / (backend + ".o")
        run([fake, "-fbackend", backend, "-flibdcext", "freestanding", "-c", source, "-o", obj])
        for os_name in ("linux", "freestanding"):
            for input_file in (source, obj):
                result = subprocess.run(["timeout", "60", str(fake), "-fbackend", backend, "-flibdcext", os_name,
                                         "-e", "_start", str(input_file), "-o", str(root / "out")], capture_output=True, text=True, timeout=65)
                filename = "libdcext-" + os_name + "-" + backend + ".a"
                assert result.returncode != 0, result
                assert "missing libdcext archive '" + filename + "'" in result.stderr, result.stderr
                assert str(root / "lib" / filename) in result.stderr, result.stderr
                assert "linker error" not in result.stderr and "linking failed" not in result.stderr, result.stderr
        run([fake, "-fbackend", backend, "-flibdcext", "linux", "-L", dcc.parent.parent / "lib",
             "-e", "_start", obj, "-o", root / "out"])
print("  RESULT  missing libdcext diagnostics passed for source and object linking on both backends")
