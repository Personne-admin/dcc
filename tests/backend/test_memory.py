#!/usr/bin/env python3
from pathlib import Path
import re
import os
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run(["timeout", "60", *map(str, command)], capture_output=True, text=True, timeout=65)
    if result.returncode:
        raise RuntimeError("%s\n%s%s" % (command, result.stdout, result.stderr))
    return result.stdout


def check_memory(archive, scratch):
    symbols = run(["nm", "-g", "--defined-only", archive])
    for name in ("memcpy", "memmove", "memset", "memcmp"):
        assert len(re.findall(r"\bT " + name + r"$", symbols, re.M)) == 1, (archive, name)
    members = run(["ar", "t", archive]).splitlines()
    memory = [member for member in members if member == "memory.asm.o"]
    assert len(memory) == 1, (archive, memory)
    obj = scratch / "memory.o"
    obj.write_bytes(subprocess.check_output(["ar", "p", archive, memory[0]], timeout=60))
    dis = run(["objdump", "-dr", obj])
    assert not re.search(r"\bcall\w*\b", dis), dis
    assert not re.search(r"%(?:xmm|ymm|zmm|mm)\d+", dis), dis
    assert not re.search(r"-0x[0-9a-f]+\(%(?:rsp|rbp)\)", dis), dis


def main():
    dcc = Path(sys.argv[1]).resolve()
    lib = dcc.parent.parent / "lib"
    source = Path(__file__).with_name("memory_exec.dc")
    windows = "--windows" in sys.argv[2:]
    os_name = "windows" if windows else "linux"
    target = "x86_64-coff" if windows else "x86_64-elf"
    cc = str(Path(os.environ.get("MINGW_SYSROOT", "/opt/llvm-mingw")) / "bin/x86_64-w64-mingw32-clang") if windows else "clang"
    with tempfile.TemporaryDirectory(prefix="dcc-memory-") as temporary:
        scratch = Path(temporary)
        calls = scratch / "calls.o"
        run([cc, "-c", "-x", "assembler-with-cpp", source.with_name("memory_calls.asm"), "-o", calls])
        for backend in ("llvm", "custom"):
            check_memory(lib / ("libdcext-" + os_name + "-" + backend + ".a"), scratch)
            for opt in ("O0", "O2"):
                exe = scratch / (backend + opt)
                obj = scratch / "test.o"
                run([dcc, "-target", target, "-flibdcext", os_name, "-fbackend", backend, "-" + opt, "-c", source, "-o", obj])
                if windows:
                    run([cc, "-nostdlib", "-Wl,--entry,_start", "-Wl,--subsystem,console", obj, calls,
                         lib / ("libdcext-windows-" + backend + ".a"), "-lkernel32", "-lws2_32", "-ladvapi32", "-lshell32", "-o", exe])
                    run([os.environ.get("WINE", "wine"), exe])
                else:
                    run([dcc, "-flibdcext", "linux", "-fbackend", backend, obj, calls, "-o", exe])
                    run([exe])
        if not windows:
            source = source.with_name("hosted_memmove.dc")
            obj = scratch / "hosted.o"
            exe = scratch / "hosted"
            run([dcc, "-flibdcext", "linux", "-fbackend", "llvm", "-O2", "-c", source, "-o", obj])
            assert "memmove" in run(["nm", "-u", obj])
            run([dcc, "-flibdcext", "linux", "-fbackend", "llvm", "-O2", source, "-o", exe])
            run([exe])
        print("  RESULT  memory execution and nonrecursive assembly passed on both backends at O0/O2")


if __name__ == "__main__":
    main()
