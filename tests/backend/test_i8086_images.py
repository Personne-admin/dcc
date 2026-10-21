from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
oracle = compiler.parent / "tests" / "i8086_link_oracle"
root = Path(__file__).resolve().parent.parent
runner = root / "i8086" / "run.py"
startup = (root / "i8086" / "crt0.dc").read_text()
checks = 0


def run(*args, expect=0, contains=None, env=None):
    global checks
    result = subprocess.run(["timeout", "60", *map(str, args)], capture_output=True, text=True, env=env)
    assert result.returncode == expect, (args, result.returncode, result.stdout, result.stderr)
    if contains is not None:
        assert contains in result.stderr, (args, result.stderr)
    checks += 1
    return result


def guest(binary, mode="real", expect=0, load="1000:0000"):
    result = subprocess.run([sys.executable, str(runner), "--load", load, "--mode", mode, str(binary)], capture_output=True, text=True)
    assert result.returncode == expect, (binary, result.returncode, result.stdout, result.stderr)


with tempfile.TemporaryDirectory(prefix="dcc-i8086-images-") as directory:
    directory = Path(directory)
    base = [compiler, "-target", "i8086-binary", "-fbackend", "custom", "-mcmodel=small"]
    plain = directory / "plain.dc"
    plain.write_text("module plain;\n@nomangle public i32 dcc_main() { return 42; }\n")
    program = directory / "program.dc"
    program.write_text(plain.read_text() + "\n" + startup)

    native = directory / "native.bin"
    run(*base, "-fbase=1000:0", program, "-o", native)
    guest(native, expect=42)

    repeat = directory / "repeat.bin"
    run(*base, "-fbase=1000:0", program, "-o", repeat)
    assert repeat.read_bytes() == native.read_bytes()
    checks += 1


    com = directory / "com.bin"
    run(*base, "-fbase=?:0x100", program, "-o", com)
    assert 0 < len(com.read_bytes()) < 0x10000
    guest(com, load="1000:0100", expect=42)
    checks += 1

    far_com = directory / "far-com.dc"
    far_com.write_text("module far_com;\n@nomangle u16 value = 19;\n@nomangle u16^ pointer = &value;\n"
                       "@nomangle public i32 dcc_main() { return *pointer as i32; }\n")
    run(*base, "-fbase=?:0x100", far_com, "-o", directory / "x.bin", expect=1,
        contains="but a static far pointer needs it")

    com_big = directory / "com-big.dc"
    com_big.write_text("module com_big;\n@nomangle u8[0xF000] buffer;\n@nomangle public i32 dcc_main() { return 42; }\n" + startup)
    run(*base, "-fbase=?:0x100", com_big, "-o", directory / "x.bin", expect=1, contains="i8086 small model")

    run(compiler, "-target", "i8086-mz", "-fbackend", "custom", program, "-o", directory / "x.exe", expect=1,
        contains="MZ image output is not implemented yet")
    run(compiler, "-target", "i8086-mz", "-fbackend", "custom", "-mcmodel=small", program, "-o", directory / "x.exe", expect=1,
        contains="MZ image output is not implemented yet")
    run(compiler, "-target", "i8086-mz", "-fbackend", "custom", "-mcmodel=unreal", plain, "-o", directory / "x.exe", expect=1,
        contains="-mcmodel=unreal is not supported for target 'i8086-mz'")
    run(compiler, "-target", "i8086-mz", "-fbackend", "custom", "-c", plain, "-o", directory / "plain-mz.o")

    run(*base, "-fno-whole-program", "-fbase=1000:0", program, "-o", directory / "x.bin", expect=1,
        contains="-fno-whole-program cannot be used with target 'i8086-binary' final images")
    run(*base, "-fno-whole-program", "-c", plain, "-o", directory / "plain.o")
    run(*base, "-fwhole-program", "-c", plain, "-o", directory / "plain.o")

    run(*base, "-T", str(directory / "x.ld"), program, "-o", directory / "x.bin", expect=1,
        contains="-T and linker scripts are not supported for i8086 final images")
    run(*base, "-Wl,--gc-sections", program, "-o", directory / "x.bin", expect=1,
        contains="-Wl, linker options are not supported for i8086 final images")
    run(*base, "-L" + str(directory), program, "-o", directory / "x.bin", expect=1, contains="library search")
    run(*base, "-lfoo", program, "-o", directory / "x.bin", expect=1, contains="library search")
    run(*base, "-shared", program, "-o", directory / "x.bin", expect=1)
    run(*base, "-o", directory / "x.a", plain, expect=1)

    object_file = directory / "plain.o"
    run(*base, "-fbase=1000:0", object_file, "-o", directory / "x.bin", expect=1,
        contains="external assembly, object or archive input")
    archive = directory / "libx.a"
    archive.write_bytes(b"!<arch>\n")
    run(*base, archive, "-o", directory / "x.bin", expect=1, contains="external assembly, object or archive input")
    assembly = directory / "x.s"
    assembly.write_text("bits 16\n")
    run(*base, assembly, "-o", directory / "x.bin", expect=1, contains="external assembly, object or archive input")
    nasm_source = directory / "x.asm"
    nasm_source.write_text("bits 16\n")
    run(*base, nasm_source, "-o", directory / "x.bin", expect=1, contains="external assembly, object or archive input")

    entry = directory / "entry.dc"
    entry.write_text("module entry;\n@nomangle public i32 dcc_main() { return 42; }\n"
                     "@nomangle public i32 later() { return 7; }\n")
    run(*base, "-e", "dcc_main", "-fbase=1000:0", entry, "-o", directory / "entry.bin")
    assert (directory / "entry.bin").exists()
    run(*base, "-e", "later", "-fbase=1000:0", entry, "-o", directory / "entry.bin", expect=1,
        contains="is not reached directly from it")
    run(*base, "-e", "missing", "-fbase=1000:0", entry, "-o", directory / "entry.bin", expect=1,
        contains="entry symbol `missing` is not defined executable code")
    jumped = directory / "jumped.dc"
    jumped.write_text("module jumped;\nasm @[intel] {\n    \\\\.section .start, \"ax\", @progbits\n    \\\\.globl _start\n"
                      "    \\\\_start:\n    \\\\jmp dcc_main\n};\n@nomangle public i32 dcc_main() { return 42; }\n")
    run(*base, "-e", "dcc_main", "-fbase=1000:0", jumped, "-o", directory / "jumped.bin")
    called = directory / "called.dc"
    called.write_text("module called;\nasm @[intel] {\n    \\\\.section .start, \"ax\", @progbits\n    \\\\.globl _start\n"
                      "    \\\\_start:\n    \\\\call dcc_main\n    \\\\mov dx, 0xf4\n    \\\\out dx, al\n    \\\\hlt\n};\n"
                      "@nomangle public i32 dcc_main() { return 42; }\n")
    run(*base, "-e", "dcc_main", "-fbase=1000:0", called, "-o", directory / "called.bin")
    guest(directory / "called.bin", expect=42)

    mid = directory / "mid.dc"
    mid.write_text("module mid;\n@nomangle public i32 compute(i32 v) { return v * 2 + 1; }\n@nomangle public u16 bias = 5;\n"
                   "@nomangle public i32 unused_helper() { return 99; }\n")
    app = directory / "app.dc"
    app.write_text("module app;\nimport mid;\nusing mid::*;\n@nomangle public i32 dcc_main() { return compute(3) + bias as i32; }\n"
                   + startup)
    import_image = directory / "import.bin"
    reach = run(*base, "-I" + str(directory), "-fbase=1000:0", "--print-reachability", app, "-o", import_image)
    assert "dropped function" in reach.stdout and "unused_helper" in reach.stdout, reach.stdout
    assert "extern _DC" not in reach.stdout
    guest(import_image, expect=12)

    arith = directory / "arith.dc"
    arith.write_text("module arith;\n"
                     "@noinline @runtime i64 signed_divide(i64 a, i64 b) { return a / b; }\n"
                     "@noinline @runtime i64 signed_remainder(i64 a, i64 b) { return a % b; }\n"
                     "@noinline @runtime u64 unsigned_divide(u64 a, u64 b) { return a / b; }\n"
                     "@noinline @runtime u64 unsigned_remainder(u64 a, u64 b) { return a % b; }\n"
                     "@nomangle public i32 dcc_main() {\n    volatile i64 a = 0x123456789abcdef0;\n    volatile u64 c = 0xfedcba9876543210;\n"
                     "    if signed_divide(a, 3) != 0x611722833944a50 { return 1; }\n"
                     "    if signed_remainder(a, 3) != 0 { return 2; }\n"
                     "    if unsigned_divide(c, 7) != 0x2468acf13579be02 { return 3; }\n"
                     "    if unsigned_remainder(c, 7) != 2 { return 4; }\n    return 42;\n}\n" + startup)
    run(*base, "-fbase=1000:0", arith, "-o", directory / "arith.bin")
    guest(directory / "arith.bin", expect=42)

    asm_helper = directory / "asm-helper.dc"
    asm_helper.write_text("module helper;\n@import @nomangle extern u16 answer();\nasm @[intel] {\n    \\\\.text\n"
                          "    \\\\.globl answer\n    \\\\answer:\n    \\\\mov ax, 42\n    \\\\ret\n};\n"
                          "@nomangle public i32 dcc_main() { return answer() as i32; }\n" + startup)
    run(*base, "-fbase=1000:0", asm_helper, "-o", directory / "asm-helper.bin")
    guest(directory / "asm-helper.bin", expect=42)

    script = run(*base, "--print-linker-script")
    assert "OUTPUT_FORMAT(binary)" in script.stdout and "__dcc_code_end = .;" in script.stdout
    com_script = run(*base, "-fbase=?:0x100", "--print-linker-script")
    assert ". = 0x100;" in com_script.stdout, com_script.stdout
    run(compiler, "-target", "i8086-mz", "--print-linker-script", expect=1, contains="applies only to target 'i8086-binary'")

    listing = directory / "program.s"
    run(*base, "-fwhole-program", "-S", program, "-o", listing)
    reassembled = directory / "program-nasm.o"
    run("nasm", "-f", "elf32", listing, "-o", reassembled)
    oracle_image = directory / "oracle.bin"
    run(oracle, "-mcmodel=small", "-fbase=1000:0", reassembled, "-o", oracle_image)
    assert oracle_image.read_bytes() == native.read_bytes(), "NASM round trip differs from the native image"
    checks += 1

    objects = directory / "objects"
    objects.mkdir()
    run(*base, "-c", program, "-o", objects / "program.o")
    run(*base, "-S", program, "-o", objects / "program.s")
    assert (objects / "program.o").exists() and (objects / "program.s").exists()

print("  RESULT  i8086 final image policy: %d/%d passed" % (checks, checks))
