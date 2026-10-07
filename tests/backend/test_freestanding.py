#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from test_memory import check_memory, run

def check_archive(archive, scratch):
    check_memory(archive, scratch)
    dis = run(["objdump", "-dr", archive])
    assert not re.search(r"%(?:xmm|ymm|zmm|mm)\d+|%st\b", dis), archive
    assert not re.search(r"-0x[0-9a-f]+\(%rsp\)", dis), archive
    functions = re.sub(r"\n[0-9a-f]+ <[^>]+\.bb[0-9]+>:\n", "\n", dis)
    for function in re.split(r"\n[0-9a-f]+ <[^>]+>:\n", functions)[1:]:
        reserved = sum(int(n, 16) for n in re.findall(r"\bsub\s+\$0x([0-9a-f]+),%rsp", function))
        pushes = len(re.findall(r"\bpush\s+%", function))
        reserved += max(pushes - 1, 0) * 8
        for offset in re.findall(r"-0x([0-9a-f]+)\(%rbp\)", function):
            assert int(offset, 16) <= reserved, (archive, offset, reserved, function)
    members = run(["ar", "t", archive]).splitlines()
    assert "crt0.asm.o" not in members, archive
    reloc = run(["readelf", "-r", archive])
    assert not re.search(r"\bR_X86_64_(?:32|32S)\b", reloc), archive


def main():
    dcc = Path(sys.argv[1]).resolve()
    sources = Path(__file__).parent
    lib = dcc.parent.parent / "lib"
    with tempfile.TemporaryDirectory(prefix="dcc-freestanding-") as temporary:
        scratch = Path(temporary)
        start = scratch / "start.s"
        start.write_text(".text\n.globl _start\n_start:\n and $-16,%rsp\n call kernel_test\n mov %eax,%edi\n mov $60,%eax\n syscall\n.section .note.GNU-stack,\"\",@progbits\n")
        start_obj = scratch / "start.o"
        run(["clang", "-c", start, "-o", start_obj])
        calls = scratch / "calls.o"
        run(["clang", "-c", "-x", "assembler-with-cpp", sources / "memory_calls.asm", "-o", calls])
        for backend in ("llvm", "custom"):
            archive = lib / ("libdcext-freestanding-" + backend + ".a")
            check_archive(archive, scratch)
            for opt in ("O0", "O2"):
                obj = scratch / (backend + opt + ".o")
                flags = [dcc, "-flibdcext", "freestanding", "-fbackend", backend, "-" + opt,
                         "-fno-simd", "-fno-x87", "-fno-red-zone", "-fpic"]
                run([*flags, "-c", sources / "freestanding_link.dc", "-o", obj])
                if backend == "llvm":
                    undefined = run(["nm", "-u", obj])
                    assert "memcpy" in undefined and "memset" in undefined, undefined
                modules_obj = scratch / "modules.o"
                run([*flags, "-c", sources / "freestanding_modules.dc", "-o", modules_obj])
                memory_obj = scratch / "memory-test.o"
                run([*flags, "-c", sources / "memory_exec.dc", "-o", memory_obj])
                harness = scratch / "memory-start.s"
                harness.write_text(start.read_text().replace("kernel_test", "memory_test"))
                harness_obj = scratch / "memory-start.o"
                run(["clang", "-c", harness, "-o", harness_obj])
                executable = scratch / "memory-exec"
                run(["ld.lld", harness_obj, memory_obj, calls, archive, "-o", executable])
                run([executable])
                for address in ("0x100000", "0xffffffff80000000"):
                    script = scratch / "kernel.ld"
                    script.write_text("ENTRY(_start)\nSECTIONS { . = " + address + "; .text : { *(.text*) } . = ALIGN(0x1000); .rodata : { *(.rodata*) *(.eh_frame*) } . = ALIGN(0x1000); .data : { *(.data*) *(.got*) } . = ALIGN(0x1000); .bss : { *(.bss*) } }\n")
                    executable = scratch / "kernel"
                    run([dcc, "-flibdcext", "freestanding", "-fbackend", backend, "-T", script,
                         "-e", "_start", start_obj, obj, "-o", executable])
                    assert int(run(["readelf", "-h", executable]).split("Entry point address:")[1].split()[0], 16) == int(address, 16)
                    if address == "0x100000":
                        run([executable])
                    whole = scratch / "kernel-whole"
                    run(["ld.lld", "-T", script, start_obj, obj, "--whole-archive", archive, "--no-whole-archive", "-o", whole])
                    if address == "0x100000":
                        run([whole])
                    modules = scratch / "modules"
                    run([dcc, "-flibdcext", "freestanding", "-fbackend", backend, "-T", script,
                         "-e", "_start", start_obj, modules_obj, "-o", modules])
                    if address == "0x100000":
                        run([modules])

    print("  RESULT  freestanding memory, low/high links and archive safety passed")


if __name__ == "__main__":
    main()
