#!/usr/bin/env python3

import argparse
from pathlib import Path
import subprocess
import tempfile


SOURCES = {
    "self_loop": "module test; public void self_loop() { for (;;) { continue; } }\n",
    "empty_cycle": "module test; public void empty_cycle() { for (;;) { if (true) { continue; } else { continue; } } }\n",
    "no_exit_edge": "module test; public void no_exit_edge() { i32 x = 0; for (;;) { x = x + 1; } }\n",
    "call_loop": "module test; void ping() {} public void call_loop() { for (;;) { ping(); } }\n",
    "terminating_call": "module main; i32 answer() { return 7; } public i32 main() { for (;;) { return answer(); } }\n",
}


def run(command, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if result.returncode != expected:
        raise RuntimeError("%s: exit %d, expected %d\n%s%s" % (" ".join(map(str, command)), result.returncode, expected, result.stdout, result.stderr))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-domtree-cycles-") as temporary:
        root = Path(temporary)
        for name, source in SOURCES.items():
            path = root / (name + ".dc")
            path.write_text(source)
            for backend in ("custom", "llvm"):
                for target in ("x86_64-elf", "x86_64-coff"):
                    for opt in ("O0", "O2"):
                        output = root / ("%s-%s-%s-%s.o" % (name, backend, target, opt))
                        run([dcc, "-fbackend", backend, "-target", target, "-" + opt, "-c", "-o", output, path])
                        checks += 1
                for opt in ("O0", "O2"):
                    if name != "terminating_call":
                        continue
                    output = root / ("%s-%s-%s" % (name, backend, opt))
                    run([dcc, "-fbackend", backend, "-target", "x86_64-elf", "-flibdcext", "linux", "-" + opt, "-o", output, path])
                    run([output], expected=7)
                    checks += 1
    print("  RESULT  %d/%d dominator cycle checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
