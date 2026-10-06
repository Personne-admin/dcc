#!/usr/bin/env python3

import argparse
from pathlib import Path
import re
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
    fixture = Path(__file__).parent.parent / "cases/sema/using-public-merge.dcc-test"
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-using-public-") as temporary:
        root = Path(temporary)
        for header, body in re.findall(r"^=== ([^\n]+?) ===\n(.*?)(?=^=== |\Z)", fixture.read_text(), re.M | re.S):
            if not header.startswith("FILE: "):
                continue
            path = root / header[6:]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body.replace("@nomangle public i32 dcc_main()", "public i32 main()"))
        sources = sorted(root.rglob("*.dc"))
        for backend in ("custom", "llvm"):
            for opt in ("O0", "O2"):
                for target in ("x86_64-elf", "x86_64-coff"):
                    objects = []
                    for index, source in enumerate(sources):
                        obj = root / (backend + "-" + opt + "-" + target + "-" + str(index) + ".o")
                        run([dcc, "-fbackend", backend, "-target", target, "-I", root, "-" + opt, "-c", "-o", obj, source])
                        objects.append(obj)
                    if target == "x86_64-elf":
                        output = root / (backend + "-" + opt)
                        run([dcc, "-fbackend", backend, "-target", target, "-flibdcext", "linux", "-o", output, *objects])
                        run([output])
                    checks += 1
    print("  RESULT  %d/%d using public merge checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
