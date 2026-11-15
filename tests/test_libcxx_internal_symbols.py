#!/usr/bin/env python3
import shutil
import subprocess
import sys
from pathlib import Path

FORBIDDEN = {
    "12__stop_state": "std::stop_token shared state",
    "20__atomic_unique_lock": "std::stop_token lock",
    "10stop_token": "std::stop_token",
    "11stop_source": "std::stop_source",
    "7jthread": "std::jthread",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_libcxx_internal_symbols.py <build-dir>", file=sys.stderr)
        return 2
    build = Path(sys.argv[1])
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if nm is None:
        print("error: nm not found", file=sys.stderr)
        return 1
    objects = sorted((build / "obj").rglob("*.o"))
    if not objects:
        print(f"error: no objects under {build / 'obj'}", file=sys.stderr)
        return 1
    hits = []
    for obj in objects:
        result = subprocess.run([nm, "--no-sort", str(obj)], capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            print(f"error: {nm} failed on {obj}: {result.stderr.strip()}", file=sys.stderr)
            return 1
        for line in result.stdout.splitlines():
            for key, what in FORBIDDEN.items():
                if key in line:
                    hits.append(f"{obj.relative_to(build)}: {what}: {line.split()[-1]}")
                    break
    if hits:
        print("error: objects depend on libc++ stop_token internals, which the pinned CI toolchain (LLVM 22.1.8) "
              "does not emit for translation units that import std; use std::thread with an explicit stop flag", file=sys.stderr)
        for hit in hits[:40]:
            print("  " + hit, file=sys.stderr)
        return 1
    print(f"  PASS     no stop_token internals in {len(objects)} objects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
