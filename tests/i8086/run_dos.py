#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--format", choices=("com", "mz"), required=True)
    parser.add_argument("--paragraph", default="0x3000")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--stdout-out", type=Path, required=True)
    parser.add_argument("--stderr-out", type=Path, required=True)
    parser.add_argument("--accel", default="tcg")
    args = parser.parse_args(argv)
    try:
        result = dos.run(args.binary.read_bytes(), args.format, paragraph=int(args.paragraph, 0), timeout=args.timeout, accel=args.accel)
    except Exception as error:
        print(str(error), file=sys.stderr)
        return 254
    if result.error:
        print(result.error, file=sys.stderr)
        return 254
    args.stdout_out.write_bytes(result.stdout)
    args.stderr_out.write_bytes(result.stderr)
    return result.exit_code if result.exit_code is not None else 254


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
