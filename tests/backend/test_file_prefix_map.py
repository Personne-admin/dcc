#!/usr/bin/env python3

import argparse
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd):
    result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    dcc = parser.parse_args().dcc.resolve()
    source = """module prefix_probe;
import core::source_location;
using core::source_location::*;
const SourceLocation LOCATION = source_location();
@nomangle public []const u8 file() { return LOCATION.file; }
@nomangle public u8 load([]u8 values, usize index) { return values[index]; }
"""
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-prefix-map-") as temporary:
        roots = [Path(temporary) / "first directory", Path(temporary) / "second"]
        for root in roots:
            root.mkdir()
            (root / "probe.dc").write_text(source)
        for backend in ("custom", "llvm"):
            for target in ("x86_64-elf", "x86_64-coff"):
                for opt in ("O0", "O2"):
                    formats = [None] if backend == "custom" else [None, "-gdwarf" if target.endswith("elf") else "-gpdb"]
                    for debug in formats:
                        for flag, extension in (("-c", ".o"), ("-S", ".s")):
                            artifacts = []
                            for root in roots:
                                output = root / ("output" + extension)
                                command = [dcc, "-fbackend", backend, "-target", target, "-" + opt, "-fbounds-check", flag, "-o", output]
                                if debug:
                                    command.append(debug)
                                run([*command, "-ffile-prefix-map=" + str(root) + "=canonical", root / "probe.dc"], root)
                                artifacts.append(output.read_bytes())
                                if flag == "-c":
                                    assert b"canonical/probe.dc" in artifacts[-1]
                                    assert str(root).encode() not in artifacts[-1]
                                    run([*command, root / "probe.dc"], root)
                                    assert str(root / "probe.dc").encode() in output.read_bytes()
                                run([*command, "-ffile-prefix-map=" + str(root / "probe.dc") + "=canonical/probe.dc", "-ffile-prefix-map=" + str(root) + "=canonical", root / "probe.dc"], root)
                                assert output.read_bytes() == artifacts[-1]
                            assert artifacts[0] == artifacts[1], (backend, target, opt, debug, flag)
                            checks += 1
        invalid = subprocess.run([dcc, "-c", "-ffile-prefix-map=invalid", roots[0] / "probe.dc"], capture_output=True, timeout=60)
        assert invalid.returncode != 0 and b"requires OLD=NEW" in invalid.stderr
        (roots[0] / "invalid.dc").write_text("module broken; public i32 f() { return missing; }")
        invalid = subprocess.run([dcc, "-c", "-ffile-prefix-map=" + str(roots[0]) + "=canonical", roots[0] / "invalid.dc"], capture_output=True, timeout=60)
        assert invalid.returncode != 0 and b"canonical/invalid.dc" in invalid.stderr
        checks += 2
    print("  RESULT  %d/%d file prefix map checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
