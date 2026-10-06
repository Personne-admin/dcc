#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    parser.add_argument("--attempts", type=int, default=10)
    parser.add_argument("--source", type=Path, action="append")
    parser.add_argument("--opt", choices=("O0", "O2"), action="append")
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    if args.attempts < 1:
        parser.error("--attempts must be positive")
    sources = args.source or [Path(__file__).with_name(name) for name in ("nttp_order.dc", "inlining_order.dc", "lowering_order.dc")]
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-codegen-determinism-") as temporary:
        root = Path(temporary)
        for source in sources:
            for target in ("x86_64-elf", "x86_64-coff"):
                for opt in args.opt or ("O0", "O2"):
                    for flag, suffix in (("-c", ".o"), ("-S", ".s")):
                        expected = None
                        for attempt in range(args.attempts):
                            output = root / ("%s-%s-%d%s" % (target, opt, attempt, suffix))
                            command = ["timeout", "60", dcc, "-fbackend", "custom", "-target", target, "-" + opt, flag, "-o", output, source]
                            result = subprocess.run(command, capture_output=True, text=True, timeout=65)
                            if result.returncode:
                                raise RuntimeError("%s: exit %d\n%s%s" % (" ".join(map(str, command)), result.returncode, result.stdout, result.stderr))
                            data = output.read_bytes()
                            if expected is None:
                                expected = data
                            elif data != expected:
                                raise RuntimeError("nondeterministic %s %s %s %s" % (source, target, opt, suffix))
                            checks += 1
                        if args.report:
                            print("  HASH    %s %s %s %s %s (%d identical)" % (source.name, target, opt, suffix, hashlib.sha256(expected).hexdigest(), args.attempts))
    print("  RESULT  %d/%d codegen determinism checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
