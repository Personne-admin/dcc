#!/usr/bin/env python3

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    source = Path(__file__).with_name("nttp_order.dc")
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-codegen-determinism-") as temporary:
        root = Path(temporary)
        for target in ("x86_64-elf", "x86_64-coff"):
            for opt in ("O0", "O2"):
                for flag, suffix in (("-c", ".o"), ("-S", ".s")):
                    expected = None
                    for attempt in range(10):
                        output = root / ("%s-%s-%d%s" % (target, opt, attempt, suffix))
                        command = [dcc, "-fbackend", "custom", "-target", target, "-" + opt, flag, "-o", output, source]
                        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                        if result.returncode:
                            raise RuntimeError("%s: exit %d\n%s%s" % (" ".join(map(str, command)), result.returncode, result.stdout, result.stderr))
                        data = output.read_bytes()
                        if expected is None:
                            expected = data
                        elif data != expected:
                            raise RuntimeError("nondeterministic %s %s %s" % (target, opt, suffix))
                        checks += 1
    print("  RESULT  %d/%d codegen determinism checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
