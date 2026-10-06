#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def run(command):
    result = subprocess.run(["timeout", "60", *map(str, command)], capture_output=True, text=True, timeout=65)
    if result.returncode:
        raise RuntimeError("%s: exit %d\n%s%s" % (" ".join(map(str, command)), result.returncode, result.stdout, result.stderr))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    parser.add_argument("--attempts", type=int, default=10)
    parser.add_argument("--source", type=Path, action="append")
    args = parser.parse_args()
    if args.attempts < 1:
        parser.error("--attempts must be positive")
    sources = args.source or [Path(__file__).with_name(name) for name in ("mem2reg_chain.dc", "mem2reg_order.dc")]
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-mem2reg-") as temporary:
        root = Path(temporary)
        for source in sources:
            for backend in ("custom", "llvm"):
                for opt in ("O0", "O2"):
                    common = [args.dcc.resolve(), "-fbackend", backend, "-" + opt]
                    executable = root / "exec"
                    run([*common, "-target", "x86_64-elf", "-flibdcext", "linux", "-o", executable, source])
                    run([executable])
                    checks += 1
                    for target in ("x86_64-elf", "x86_64-coff"):
                        for flag, suffix in (("-c", ".o"), ("-S", ".s")):
                            output = root / ("artifact" + suffix)
                            hashes = set()
                            for attempt in range(args.attempts):
                                run([*common, "-target", target, flag, "-o", output, source])
                                hashes.add(hashlib.sha256(output.read_bytes()).hexdigest())
                                checks += 1
                            if len(hashes) != 1:
                                raise RuntimeError("%s %s %s %s %s: %d hashes" % (source, backend, target, opt, suffix, len(hashes)))
    print("  RESULT  %d/%d mem2reg checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
