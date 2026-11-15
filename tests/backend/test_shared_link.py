#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DCC = Path(sys.argv[1]).resolve()
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
READELF = shutil.which("llvm-readelf") or shutil.which("readelf")
failures = []
checks = 0


def check(condition, label, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(label)
        print(f"  FAIL     {label}")
        if detail:
            print("           " + detail.replace("\n", "\n           "))


def run(argv, cwd=None, env=None, timeout=120):
    merged = dict(os.environ)
    if env:
        merged.update(env)
    return subprocess.run([str(a) for a in argv], cwd=cwd, env=merged, capture_output=True, text=True, timeout=timeout)


def dcc(*args, cwd=None, env=None):
    return run([DCC, *args], cwd=cwd, env=env)


def readelf(*args):
    return run([READELF, "-W", *args]).stdout


def cbuild_shared(directory, name, source):
    directory.mkdir(parents=True, exist_ok=True)
    c_file = directory / f"{name}.c"
    c_file.write_text(source)
    library = directory / f"lib{name}.so"
    result = run([CC, "-shared", "-fPIC", "-o", library, c_file])
    if result.returncode != 0:
        raise SystemExit(f"error: cannot build the C shared library: {result.stderr}")
    return library


def test_custom_shared_library_output(root):
    out = root / "shared out" / "libplugin.so"
    out.parent.mkdir(parents=True)
    source = root / "plugin.dc"
    source.write_text("module plugin;\n\n@nomangle public i32 plugin_twice(i32 x) {\n    return x * 2;\n}\n")
    r = dcc(source, "-shared", "-fPIC", "-fbackend=custom", "-o", out)
    check(r.returncode == 0, "custom backend writes a shared library into a path with spaces", r.stderr)
    if r.returncode != 0:
        return
    check("DYN (Shared object file)" in readelf("-h", out), "the custom shared library is ET_DYN")
    user = root / "use_plugin.c"
    user.write_text("int plugin_twice(int);\nint main(void) { return plugin_twice(21) == 42 ? 0 : 1; }\n")
    exe = root / "use_plugin"
    r = run([CC, user, "-o", exe, f"-L{out.parent}", "-lplugin", f"-Wl,-rpath,{out.parent}"])
    check(r.returncode == 0, "a C program links against the dcc shared library", r.stderr)
    if r.returncode == 0:
        check(run([exe]).returncode == 0, "the C program calls into the dcc shared library")


def main():
    if CC is None or READELF is None:
        print("error: test_shared_link.py needs a host C compiler and readelf", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="dcc shared link ") as directory:
        root = Path(directory)
        test_custom_shared_library_output(root)
    if failures:
        print(f"  FAIL     test_shared_link: {len(failures)} of {checks} checks failed")
        return 1
    print(f"  PASS     test_shared_link ({checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
