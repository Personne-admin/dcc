#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True, timeout=60).stdout


def main():
    repo = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="dcc-archive-members-") as temporary:
        scratch = Path(temporary)
        objects = []
        for name in ("first", "second"):
            source = scratch / (name + ".c")
            source.write_text("int %s(void) { return 1; }\n" % name)
            output = scratch / (name + ".o")
            run([os.environ.get("CC", "clang"), "-c", str(source), "-o", str(output)], repo)
            objects.append(output)
        compiler = scratch / "compiler"
        compiler.touch()
        checks = 0
        for component in ("compiler", "libdcext"):
            archive = scratch / (component + ".a")
            command = [
                "make", "--no-print-directory", "-s", "-f", component + "/GNUmakefile", str(archive),
                "RULES_MK=" + str(repo / "mk/rules.mk"), "STD_MK=/dev/null", "COMPDB_MK=/dev/null",
                "MODULE_SRCS=", "ALL_MODULE_SRCS=", "CC_SRCS=", "CC_OBJS=", "DC_SRCS=", "ASM_SRCS=",
                "DC_OBJS=", "ASM_OBJS=", "LIB=" + str(archive), "LIBDCEXT_A=" + str(archive),
                "BUILD_DIR=" + str(scratch / component), "DCC=" + str(compiler), "Q=@",
                "AR=" + os.environ.get("AR", "ar"),
            ]
            for inputs in (objects, objects[1:], list(reversed(objects)), objects):
                argument = "ALL_OBJS=" + " ".join(map(str, inputs))
                run(command + [argument], repo)
                members = run([os.environ.get("AR", "ar"), "t", str(archive)], repo).splitlines()
                expected = [path.name for path in inputs]
                if members != expected:
                    raise RuntimeError("%s members %s do not match requested %s" % (component, members, expected))
                checks += 1
                before = archive.stat().st_mtime_ns
                run(command + [argument], repo)
                if archive.stat().st_mtime_ns != before:
                    raise RuntimeError("an unchanged %s archive was rebuilt" % component)
                checks += 1
        print("  RESULT  %d/%d archive membership checks passed" % (checks, checks))


if __name__ == "__main__":
    main()
