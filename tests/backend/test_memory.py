#!/usr/bin/env python3
from pathlib import Path
import re
import os
import subprocess
import sys
import tempfile
from process_output import run_output


def run(command):
    return run_output(["timeout", "60", *map(str, command)])


def check_memory(archive, scratch):
    symbols = run(["nm", "-g", "--defined-only", archive])
    for name in ("memcpy", "memmove", "memset", "memcmp"):
        assert len(re.findall(r"\bT " + name + r"$", symbols, re.M)) == 1, (archive, name)
    members = run(["ar", "t", archive]).splitlines()
    assert "memory.asm.o" not in members, (archive, members)
    for name in ("memcpy", "memmove", "memset", "memcmp"):
        member = name + ".dc.o"
        assert members.count(member) == 1, (archive, member, members)
        obj = scratch / member
        obj.write_bytes(subprocess.check_output(["ar", "p", archive, member], timeout=60))
        check_leaf(obj, name)


def check_leaf(obj, name):
    symbols = run(["nm", "-g", "--defined-only", obj])
    assert re.findall(r"\bT (\S+)$", symbols, re.M) == [name], (obj, symbols)
    assert not run(["nm", "-u", obj]).strip(), obj
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
            for prefix in ("libdcext-",):
                check_memory(lib / (prefix + os_name + "-" + backend + ".a"), scratch)
            reference = {}
            for opt in ("O0", "O2"):
                runtime = scratch / ("libmemory-" + backend + opt + ".a")
                runtime.write_bytes((lib / ("libdcext-" + os_name + "-" + backend + ".a")).read_bytes())
                for name in ("memcpy", "memmove", "memset", "memcmp"):
                    obj = scratch / (name + ".dc.o")
                    source_leaf = Path(__file__).resolve().parents[2] / 'libdcext/common/memory' / (name + '.dc')
                    run([dcc, '-target', target, '-fbackend', backend, '-' + opt, '-c', source_leaf, '-o', obj])
                    check_leaf(obj, name)
                    data = scratch / 'leaf.bin'
                    run(['objcopy', '-O', 'binary', '--only-section=.text.memory.' + name, obj, data])
                    assert data.stat().st_size > 0
                    if name in reference:
                        assert data.read_bytes() == reference[name], (backend, name, 'O0/O2 differ')
                    reference[name] = data.read_bytes()
                    run(['ar', 'rcs', runtime, obj])
                for whole in (False, True):
                    exe = scratch / (backend + opt + str(whole) + (".exe" if windows else ""))
                    obj = scratch / "test.o"
                    run([dcc, "-target", target, "-flibdcext", os_name, "-fbackend", backend, "-" + opt,
                         *(['-fwhole-program'] if whole else []), '-c', source, '-o', obj])
                    system = Path(os.environ.get("MINGW_SYSROOT", "/opt/llvm-mingw")) / 'x86_64-w64-mingw32/lib'
                    system_flags = ['-L', system, '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32'] if windows else []
                    run([dcc, '-target', target, '-flibdcext', os_name, '-fbackend', backend,
                         *(['-fwhole-program'] if whole else []), obj, calls, '-L', scratch, '-lmemory-' + backend + opt, *system_flags, '-o', exe])
                    run([*([os.environ.get("WINE", "wine")] if windows else []), exe])
                    production = scratch / ('production.exe' if windows else 'production')
                    if windows:
                        run([dcc, '-target', target, '-flibdcext', os_name, '-fbackend', backend,
                             *(['-fwhole-program'] if whole else []), obj, calls, *system_flags, '-o', production])
                        run([os.environ.get('WINE', 'wine'), production])
                    elif whole:
                        run(['ld.lld', '--static', '--no-dynamic-linker', '--fatal-warnings', '--undefined=_start', obj, calls, '-o', production])
                        run([production])
                    else:
                        run([dcc, '-flibdcext', 'linux', '-fbackend', backend, obj, calls, '-o', production])
                        run([production])
                    print('  PASS memory', os_name, backend, opt, 'whole' if whole else 'separate', flush=True)
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
