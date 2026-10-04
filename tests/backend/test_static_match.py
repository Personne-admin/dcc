#!/usr/bin/env python3

import argparse
from pathlib import Path
import subprocess
import tempfile


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise RuntimeError("%s: exit %d\n%s%s" % (" ".join(map(str, command)), result.returncode, result.stdout, result.stderr))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    source = Path(__file__).with_name("static_match_exec.dc")
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-static-match-") as temporary:
        root = Path(temporary)
        for backend in ("custom", "llvm"):
            for opt in ("O0", "O2"):
                output = root / (backend + "-" + opt)
                run([dcc, "-fbackend", backend, "-target", "x86_64-elf", "-flibdcext", "linux", "-" + opt, "-o", output, source])
                run([output])
                checks += 1
            for opt in ("O0", "O2"):
                output = root / (backend + "-coff-" + opt + ".o")
                run([dcc, "-fbackend", backend, "-target", "x86_64-coff", "-" + opt, "-c", "-o", output, source])
                checks += 1
    print("  RESULT  %d/%d static match checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
