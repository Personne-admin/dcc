#!/usr/bin/env python3

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dcc", type=Path)
    dcc = parser.parse_args().dcc.resolve()
    repo = Path(__file__).resolve().parents[2]
    checks = 0
    with tempfile.TemporaryDirectory(prefix="dcc-library-prefix-") as temporary:
        roots = [Path(temporary) / "first", Path(temporary) / "second"]
        for root in roots:
            (root / "libdcext/std").mkdir(parents=True)
            (root / "mk").mkdir()
            (root / "libdcext/common").mkdir()
            shutil.copyfile(repo / "libdcext/common/abi.dc", root / "libdcext/common/abi.dc")
            shutil.copyfile(repo / "mk/llvm.mk", root / "mk/llvm.mk")
            (root / "libdcext/std.dc").write_text("module std;\n")
            (root / "libdcext/assert.dc").write_text("module assert; import core::source_location; const core::source_location::SourceLocation LOC = core::source_location::source_location(); @nomangle public []const u8 file() { return LOC.file; }\n")
        for backend in ("custom", "llvm"):
            for target in ("x86_64-elf", "x86_64-coff"):
                artifacts = []
                for root in roots:
                    build = root / (backend + "-" + target)
                    archive = build / "library.a"
                    command = ["make", "--no-print-directory", "-s", "-f", str(repo / "libdcext/GNUmakefile"), str(archive),
                               "RULES_MK=" + str(repo / "mk/rules.mk"), "CONFIG_MK=" + str(repo / "mk/config.mk"),
                               "TOPLEVEL=" + str(root), "BUILD_DIR=" + str(build), "OBJ_DIR=" + str(build / "obj"),
                               "DEP_DIR=" + str(build / "dep"), "NATIVE_BUILD_DIR=" + str(repo / "build"),
                               "LIBDCEXT_A=" + str(archive), "DCC=" + str(dcc), "DCC_WRAPPER=timeout 60",
                               "FILE_PREFIX_MAP_FLAGS=-ffile-prefix-map=" + str(root) + "=dcc",
                               "TARGET=" + target, "BACKEND=" + backend, "ASM_SRCS=", "AR=ar", "CXX=clang++", "CC=clang"]
                    run(command, root)
                    mapped = archive.read_bytes()
                    assert b"dcc/libdcext/assert.dc" in mapped and str(root).encode() not in mapped
                    timestamp = archive.stat().st_mtime_ns
                    run(command, root)
                    assert archive.stat().st_mtime_ns == timestamp
                    run(command + ["FILE_PREFIX_MAP_FLAGS="], root)
                    assert str(root / "libdcext/assert.dc").encode() in archive.read_bytes()
                    run(command, root)
                    assert archive.read_bytes() == mapped
                    artifacts.append(mapped)
                    checks += 4
                assert artifacts[0] == artifacts[1], (backend, target)
                checks += 1
    print("  RESULT  %d/%d libdcext prefix map checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
