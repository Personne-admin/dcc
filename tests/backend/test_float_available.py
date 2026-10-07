#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile
from test_memory import run

with tempfile.TemporaryDirectory(prefix="dcc-float-target-") as temporary:
    root = Path(temporary)
    (root / "std").mkdir()
    (root / "std/math.dc").write_text('''module std::math;
import core::target;
public u64 integer(u64 value) { return value + 1; }
static if core::target::FLOAT_AVAILABLE {
    public struct Probe { f64 value; }
    public using Alias = Probe;
    public f64 floating(const Probe* value) { return value.value + 1.0; }
}
''')
    source = root / "main.dc"
    source.write_text('''module main;
import core;
import core::target;
import std::math;
using std::math;
@nomangle public i32 gate_test() {
    if (4 as u64).integer() != 5 { return 1; }
    static if core::target::FLOAT_AVAILABLE {
        math::Alias value = { value = 3.0 };
        if value.floating() != 4.0 { return 2; }
    } else {
        static if compiles { math::Probe value; } { core::compile_error("struct leaked"); }
        static if compiles { math::Alias value; } { core::compile_error("alias leaked"); }
        static if compiles { math::Probe value; value.floating(); } { core::compile_error("UFCS leaked"); }
        static if compiles { math::floating(null); } { core::compile_error("function leaked"); }
    }
    return 0;
}
''')
    start = root / "start.s"
    start.write_text('.text\n.globl _start\n_start:\n call gate_test\n mov %eax,%edi\n mov $60,%eax\n syscall\n.section .note.GNU-stack,"",@progbits\n')
    run(["clang", "-c", start, "-o", root / "start.o"])
    checks = 0
    for backend in ("llvm", "custom"):
        for opt in ("O0", "O2"):
            for simd in (True, False):
                for x87 in (True, False):
                    flags = [sys.argv[1], "-target", "x86_64-elf", "-fbackend", backend, "-" + opt,
                             "-fsimd" if simd else "-fno-simd", "-fx87" if x87 else "-fno-x87"]
                    objects = [root / "math.o", root / "main.o"]
                    for src, obj in zip((root / "std/math.dc", source), objects):
                        run([*flags, "-c", src, "-o", obj])
                    run(["ld.lld", root / "start.o", *objects, "-o", root / "test"])
                    run([root / "test"])
                    checks += 1
    print("  RESULT  %d floating-point availability and declaration/import/UFCS checks passed" % checks)
