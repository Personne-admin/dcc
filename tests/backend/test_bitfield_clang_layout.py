#!/usr/bin/env python3

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


NAMES = ("single", "several", "nofit", "zerosep", "mixed", "signed", "packed", "wide", "bool")

PAD = {"mixed": (5, 6, 7), "bool": (1, 2, 3)}


def masked(name, payload):
    data = bytearray(payload)
    for index in PAD.get(name, ()):
        data[index] = 0
    return bytes(data)


def run(command):
    result = subprocess.run(["timeout", "60", *map(str, command)], capture_output=True, timeout=65)
    if result.returncode:
        raise RuntimeError(
            "%s: exit %d\n%s%s"
            % (" ".join(map(str, command)), result.returncode, result.stdout.decode("utf-8", "replace"), result.stderr.decode("utf-8", "replace"))
        )
    return result.stdout


def parse(stream):
    cases = []
    offset = 0
    for name in NAMES:
        size, align = struct.unpack_from("<QQ", stream, offset)
        offset += 16
        cases.append((name, size, align, stream[offset : offset + size]))
        offset += size
    if offset != len(stream):
        raise RuntimeError("trailing %d bytes" % (len(stream) - offset))
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    parser.add_argument("--clang", default="clang")
    args = parser.parse_args()
    here = Path(__file__).parent
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-bitfield-clang-") as temporary:
        root = Path(temporary)
        reference = root / "ref"
        run([args.clang, here / "bitfield_clang_layout.c", "-o", reference])
        expected = parse(run([reference]))
        for backend in ("llvm", "custom"):
            for opt in ("O0", "O2"):
                candidate = root / "cand"
                run([args.dcc.resolve(), "-fbackend", backend, "-" + opt, "-target", "x86_64-elf", "-flibdcext", "linux",
                     "-o", candidate, here / "bitfield_clang_layout.dc"])
                actual = parse(run([candidate]))
                for want, got in zip(expected, actual):
                    want_masked = (want[0], want[1], want[2], masked(want[0], want[3]))
                    got_masked = (got[0], got[1], got[2], masked(got[0], got[3]))
                    if want_masked != got_masked:
                        raise RuntimeError(
                            "%s %s %s: clang=%r dcc=%r" % (backend, opt, want[0], want, got)
                        )
                    checks += 1
    print("  RESULT  %d/%d bitfield clang layout checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
