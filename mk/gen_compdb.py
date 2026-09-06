#!/usr/bin/env python3
import argparse
import json
import os
import sys


def cmd_fragment(args):
    flags = args.cxxflags.split()
    toplevel = os.path.abspath(args.toplevel)

    entries = []

    for src in args.sources:
        src = os.path.abspath(src)

        if args.mode == "plain":
            cmd_parts = [args.cxx] + flags + [src]
        else:
            pcm_dir = os.path.abspath(args.pcm_dir)
            ext = os.path.splitext(src)[1]

            cmd_parts = [args.cxx] + flags + [f"-fprebuilt-module-path={pcm_dir}"]

            if ext == ".cppm":
                cmd_parts += ["--precompile", src]
            else:
                cmd_parts += ["-c", src]

        entries.append(
            {
                "directory": toplevel,
                "file": src,
                "arguments": cmd_parts,
            }
        )

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    with open(args.output, "w") as f:
        json.dump(entries, f, indent=2)

    count = len(entries)
    name = os.path.basename(args.output)
    print(f"  COMPDB    {count} entries -> {name}", file=sys.stderr)


def cmd_merge(args):
    all_entries = []
    seen = set()

    for frag in args.fragments:
        with open(frag) as f:
            entries = json.load(f)
        for entry in entries:
            key = entry["file"]
            if key not in seen:
                seen.add(key)
                all_entries.append(entry)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    with open(args.output, "w") as f:
        json.dump(all_entries, f, indent=2)

    print(
        f"  COMPDB    {len(all_entries)} total entries -> {args.output}",
        file=sys.stderr,
    )


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    frag = sub.add_parser("fragment")
    frag.add_argument("--cxx", required=True)
    frag.add_argument("--cxxflags", required=True)
    frag.add_argument("--mode", choices=["cxx", "plain"], default="cxx")
    frag.add_argument("--pcm-dir", default="", help="required unless --mode plain")
    frag.add_argument("--toplevel", required=True)
    frag.add_argument("--output", required=True)
    frag.add_argument("sources", nargs="*", default=[])

    merge = sub.add_parser("merge")
    merge.add_argument("--output", required=True)
    merge.add_argument("fragments", nargs="*", default=[])

    args = parser.parse_args()

    if args.command == "fragment":
        cmd_fragment(args)
    elif args.command == "merge":
        cmd_merge(args)


if __name__ == "__main__":
    main()
