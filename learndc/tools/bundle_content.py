#!/usr/bin/env python3
import argparse
import struct
import sys
from pathlib import Path

ALLOWED = {".toml", ".md", ".dc", ".dcon", ".txt"}
MAGIC = b"LDCB1\n"


def collect(root: Path) -> list[tuple[str, bytes]]:
    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise SystemExit(f"error: {path}: symbolic links are not allowed in learndc content")
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        parts = rel.split("/")
        if any(part.startswith(".") for part in parts[:-1]) or (parts[-1].startswith(".") and parts[-1] != ".gitignore"):
            continue
        if path.suffix not in ALLOWED and path.name != ".gitignore":
            raise SystemExit(f"error: {path}: unsupported content file type (allowed: {', '.join(sorted(ALLOWED))})")
        entries.append((rel, path.read_bytes()))
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack learndc content into the embedded archive")
    parser.add_argument("--root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(args.root)
    if not (root / "catalog.toml").is_file():
        print(f"error: {root}/catalog.toml not found", file=sys.stderr)
        return 1
    blob = bytearray(MAGIC)
    for rel, data in collect(root):
        name = rel.encode("utf-8")
        blob += struct.pack("<I", len(name)) + name + struct.pack("<I", len(data)) + data
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_bytes(bytes(blob))
    if out.exists() and out.read_bytes() == bytes(blob):
        tmp.unlink()
    else:
        tmp.replace(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
