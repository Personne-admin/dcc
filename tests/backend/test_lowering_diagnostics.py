#!/usr/bin/env python3

import argparse
from pathlib import Path
import subprocess
import tempfile


SOURCES = {
    "static_match": "module test; void f() { static match 2 { 1 => 10, 2 => 20, _ => 30, } }\n",
    "template_instance": "module test; T copy(T)(T value) { return value; } void f() { copy!i32; }\n",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-lowering-diagnostics-") as temporary:
        root = Path(temporary)
        for name, source in SOURCES.items():
            path = root / (name + ".dc")
            path.write_text(source)
            for backend in ("custom", "llvm"):
                for target in ("x86_64-elf", "x86_64-coff"):
                    for opt in ("O0", "O2"):
                        output = root / ("%s-%s-%s-%s.o" % (name, backend, target, opt))
                        result = subprocess.run(
                            [dcc, "-fbackend", backend, "-target", target, "-" + opt, "-c", "-o", output, path],
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )
                        if result.returncode != 1 or "internal error during IR lowering: unimplemented" not in result.stderr or " at file=" not in result.stderr:
                            raise RuntimeError("%s: exit %d\n%s%s" % (name, result.returncode, result.stdout, result.stderr))
                        checks += 1
    print("  RESULT  %d/%d lowering diagnostic checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
