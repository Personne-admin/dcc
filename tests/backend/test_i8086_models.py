from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


compiler = Path(sys.argv[1]).resolve()
oracle = compiler.parent / "tests" / "i8086_link_oracle"
root = Path(__file__).resolve().parent.parent
startup = (root / "i8086/crt0.dc").read_text()
checks = 0


def link(model, inputs, output):
    result = subprocess.run(["timeout", "60", str(oracle), "-mcmodel=" + model, "-fbase=1000:0000", *map(str, inputs), "-o", str(output)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def compile_object(model, level, source, output):
    result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level,
                             "-c", str(source), "-o", str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def compile_assembly(model, level, source, output):
    result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level,
                             "-S", str(source), "-o", str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def round_trip(model, level, plain_source, startup_source, prefix, directory):
    global checks
    plain = directory / (prefix + "-plain.o")
    compile_object(model, level, plain_source, plain)
    listing = directory / (prefix + ".s")
    compile_assembly(model, level, startup_source, listing)
    linked = directory / (prefix + "-startup.o")
    compile_object(model, level, startup_source, linked)
    reassembled = directory / (prefix + "-nasm.o")
    result = subprocess.run(["nasm", "-f", "elf32", str(listing), "-o", str(reassembled)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    images = []
    for item in (linked, reassembled):
        image = item.with_suffix(".bin")
        link(model, [item], image)
        images.append(image.read_bytes())
    assert images[0] == images[1], (model, level)
    checks += 1
    return plain, linked.with_suffix(".bin")


with tempfile.TemporaryDirectory(prefix="dcc-i8086-models-") as directory:
    directory = Path(directory)
    source_text = ("module m;\nvolatile i8 a = -100;\nvolatile u16 b = 60000;\nvolatile i32 c = -7;\nvolatile bool f;\n"
                   "@nomangle public i32 dcc_main() {\n    i8 x = a;\n    u16 y = b;\n    i32 z = c;\n    f = x < 3;\n"
                   "    i32 r = (x / 3) as i32 + (y % 1000) as i32 + z * z + (y >> 4) as i32 - (x >> 2) as i32 + (~y) as i32;\n"
                   "    return r + (x as u8 == 156) as i32 + (f as i32) - (z << (y & 7) as i32) + c_side(-4, r) + twice(r, 1, 2, 3, false);\n}\n"
                   "i32 twice(i32 v, u8 w, i16 x, u16 y, bool z) { return v * 2 + w as i32 - x as i32 + y as i32 + z as i32; }\n"
                   "@nomangle public i32 c_side(i16 a, i32 b) { return twice(b, 3, a, 9, true); }\n"
                   "public u16 value() { return 7; }\npublic bool flag() { return true; }\npublic void nothing() {}\npublic i16 counter = 3;\n")
    source = directory / "main.dc"
    source.write_text(source_text)
    program = directory / "program.dc"
    program.write_text(source_text + "\n" + startup)

    for target, model, arch, message in (
        ("i8086-binary", "unreal", "8086", "-mcmodel=unreal requires -farch i386 or newer; got -farch 8086"),
        ("i8086-binary", "unreal32", "i286", "-mcmodel=unreal32 requires -farch i386 or newer; got -farch i286"),
        ("i8086-binary", "large", "i386", "-mcmodel=large is not supported for target 'i8086-binary'"),
        ("x86_64-elf", "unreal", "generic", "-mcmodel=unreal is not supported for target 'x86_64-elf'"),
        ("x86-elf", "unreal32", "i386", "-mcmodel=unreal32 is not supported for target 'x86-elf'"),
    ):
        result = subprocess.run(["timeout", "60", str(compiler), "-target", target, "-mcmodel", model,
                                 "-farch", arch, "-c", str(source), "-o", str(directory / "out.o")], capture_output=True, text=True)
        assert result.returncode == 1 and message in result.stderr, result.stderr
        checks += 1

    for model in ("default", "small", "unreal", "unreal32"):
        for level in ("-O0", "-O2"):
            round_trip(model, level, source, program, model + level, directory)

    fixture = root / "cases/i8086/memory-aggregates-small.dcc-test"
    memory_text = fixture.read_text().split("=== FILE: main.dc ===\n", 1)[1].split("\n=== EXPECT-I8086-RUN ===", 1)[0]
    memory = directory / "memory.dc"
    memory.write_text(memory_text)
    memory_program = directory / "memory-program.dc"
    memory_program.write_text(memory_text + "\n" + startup)
    for model in ("small", "unreal", "unreal32"):
        for level in ("-O0", "-O2"):
            plain, _ = round_trip(model, level, memory, memory_program, "memory-" + model + level, directory)
            if model == "small":
                disassembly = subprocess.run(["objdump", "-d", "-M", "i8086", str(plain)], capture_output=True, text=True, check=True).stdout
                for line in disassembly.splitlines():
                    match = re.match(r"\s*[0-9a-f]+:\s*((?:[0-9a-f]{2}\s+)+)", line)
                    if match:
                        for byte in bytes.fromhex(match[1]):
                            if byte not in (0xf0, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67):
                                break
                            assert byte != 0x67, line

    for model in ("unreal", "unreal32"):
        fixture = root / f"cases/i8086/large-data-{model}.dcc-test"
        folder = directory / ("large-" + model)
        folder.mkdir()
        files = re.findall(r"=== FILE: (.*?) ===\n(.*?)(?=\n=== |\Z)", fixture.read_text(), re.S)
        for name, body in files:
            (folder / name).write_text(body)
        program_path = folder / "program.dc"
        program_path.write_text((folder / "main.dc").read_text() + "\n" + startup)
        for level in ("-O0", "-O2"):
            _, image = round_trip(model, level, folder / "main.dc", program_path, model + level, folder)
            accelerators = ("tcg", "kvm") if os.access("/dev/kvm", os.R_OK | os.W_OK) else ("tcg",)
            for accel in accelerators:
                result = subprocess.run([sys.executable, str(root / "i8086/run.py"), str(image),
                                         "--mode", "unreal", "--accel", accel], capture_output=True, timeout=60)
                assert result.returncode == 42, (model, level, accel, result.stderr)
                checks += 1
                print(f"  NOTE    large-data {model} {level} passed under {accel}")
    print("  NOTE    KVM available" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "  NOTE    KVM unavailable; QEMU TCG only")
print(f"  RESULT  i8086 model diagnostics and assembly round trips: {checks}/{checks} passed")
