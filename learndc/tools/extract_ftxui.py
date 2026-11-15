#!/usr/bin/env python3
import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

MEMBERS = ("ftxui.hpp", "ftxui.cpp")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify and extract the pinned FTXUI amalgamation")
    parser.add_argument("--zip", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    data = Path(args.zip).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != args.sha256:
        print(f"error: {args.zip}: sha256 {digest} does not match the pinned {args.sha256}", file=sys.stderr)
        return 1
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.zip) as archive:
        names = {Path(n).name: n for n in archive.namelist() if not n.endswith("/")}
        for member in MEMBERS:
            if member not in names:
                print(f"error: {args.zip} has no {member}", file=sys.stderr)
                return 1
            target = out / member
            tmp = target.with_suffix(target.suffix + ".tmp")
            tmp.write_bytes(archive.read(names[member]))
            tmp.replace(target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
