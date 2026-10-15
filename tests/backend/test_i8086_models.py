from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


compiler = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="dcc-i8086-models-") as directory:
    directory = Path(directory)
    source = directory / "main.dc"
    source.write_text("module m;\nvolatile i8 a = -100;\nvolatile u16 b = 60000;\nvolatile i32 c = -7;\nvolatile bool f;\n"
                      "@nomangle public i32 dcc_main() {\n    i8 x = a;\n    u16 y = b;\n    i32 z = c;\n    f = x < 3;\n"
                      "    i32 r = (x / 3) as i32 + (y % 1000) as i32 + z * z + (y >> 4) as i32 - (x >> 2) as i32 + (~y) as i32;\n"
                      "    return r + (x as u8 == 156) as i32 + (f as i32) - (z << (y & 7) as i32) + c_side(-4, r) + twice(r, 1, 2, 3, false);\n}\n"
                      "i32 twice(i32 v, u8 w, i16 x, u16 y, bool z) { return v * 2 + w as i32 - x as i32 + y as i32 + z as i32; }\n"
                      "@nomangle public i32 c_side(i16 a, i32 b) { return twice(b, 3, a, 9, true); }\n"
                      "public u16 value() { return 7; }\npublic bool flag() { return true; }\npublic void nothing() {}\npublic i16 counter = 3;\n")
    crt0 = directory / "crt0.o"
    result = subprocess.run(["nasm", "-f", "elf32", str(Path(__file__).resolve().parent.parent / "i8086" / "crt0.asm"), "-o", str(crt0)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    checks = 0
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
            base = ["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level]
            obj = directory / f"{model}{level}.o"
            listing = directory / f"{model}{level}.s"
            reassembled = directory / f"{model}{level}-nasm.o"
            for flag, output in (("-c", obj), ("-S", listing)):
                result = subprocess.run(base + [flag, str(source), "-o", str(output)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
            result = subprocess.run(["nasm", "-f", "elf32", str(listing), "-o", str(reassembled)], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            images = []
            for linked in (obj, reassembled):
                image = linked.with_suffix(".bin")
                result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbase=1000:0", str(crt0), str(linked),
                                         "-o", str(image)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
                images.append(image.read_bytes())
            assert images[0] == images[1], (model, level)
            checks += 1
    fixture = Path(__file__).resolve().parent.parent / "cases/i8086/memory-aggregates-small.dcc-test"
    memory = directory / "memory.dc"
    memory.write_text(fixture.read_text().split("=== FILE: main.dc ===\n", 1)[1].split("\n=== EXPECT-I8086-RUN ===", 1)[0])
    for model in ("small", "unreal", "unreal32"):
        for level in ("-O0", "-O2"):
            base = ["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level]
            obj = directory / f"memory-{model}{level}.o"
            listing = obj.with_suffix(".s")
            reassembled = directory / f"memory-{model}{level}-nasm.o"
            for flag, output in (("-c", obj), ("-S", listing)):
                result = subprocess.run(base + [flag, str(memory), "-o", str(output)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
            disassembly = subprocess.run(["objdump", "-d", "-M", "i8086", str(obj)], capture_output=True, text=True, check=True).stdout
            if model == "small":
                for line in disassembly.splitlines():
                    match = re.match(r"\s*[0-9a-f]+:\s*((?:[0-9a-f]{2}\s+)+)", line)
                    if match:
                        for byte in bytes.fromhex(match[1]):
                            if byte not in (0xf0, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67):
                                break
                            assert byte != 0x67, line
            result = subprocess.run(["timeout", "60", "nasm", "-f", "elf32", str(listing), "-o", str(reassembled)], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            images = []
            for linked in (obj, reassembled):
                image = linked.with_suffix(".bin")
                result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbase=1000:0", str(crt0), str(linked),
                                         "-o", str(image)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
                images.append(image.read_bytes())
            assert images[0] == images[1], (model, level)
            checks += 1
    for model in ("unreal", "unreal32"):
        fixture = Path(__file__).resolve().parent.parent / f"cases/i8086/large-data-{model}.dcc-test"
        folder = directory / ("large-" + model)
        folder.mkdir()
        files = re.findall(r"=== FILE: (.*?) ===\n(.*?)(?=\n=== |\Z)", fixture.read_text(), re.S)
        for name, body in files:
            (folder / name).write_text(body)
        helpers = []
        for name, body in files:
            if name.endswith(".asm"):
                obj = folder / (name + ".o")
                subprocess.run(["timeout", "60", "nasm", "-f", "elf32", str(folder / name), "-o", str(obj)], check=True)
                helpers.append(str(obj))
        for level in ("-O0", "-O2"):
            obj = folder / (level + ".o")
            listing = obj.with_suffix(".s")
            reassembled = folder / (level + "-nasm.o")
            base = ["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level]
            for flag, output in (("-c", obj), ("-S", listing)):
                result = subprocess.run(base + [flag, str(folder / "main.dc"), "-o", str(output)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
            subprocess.run(["timeout", "60", "nasm", "-f", "elf32", str(listing), "-o", str(reassembled)], check=True)
            images = []
            for linked in (obj, reassembled):
                image = linked.with_suffix(".bin")
                subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbase=1000:0",
                                str(crt0), str(linked), *helpers, "-o", str(image)], check=True)
                images.append(image.read_bytes())
            assert images[0] == images[1], (model, level)
            checks += 1
            accelerators = ("tcg", "kvm") if os.access("/dev/kvm", os.R_OK | os.W_OK) else ("tcg",)
            for accel in accelerators:
                result = subprocess.run([sys.executable, str(Path(__file__).resolve().parent.parent / "i8086/run.py"), str(obj.with_suffix(".bin")),
                                         "--mode", "unreal", "--accel", accel], capture_output=True, timeout=60)
                assert result.returncode == 42, (model, level, accel, result.stderr)
                checks += 1
                print(f"  NOTE    large-data {model} {level} passed under {accel}")
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        for model in ("small", "unreal"):
            fixture = Path(__file__).resolve().parent.parent / f"cases/i8086/memory-wrap-{model}.dcc-test"
            source = directory / f"wrap-{model}.dc"
            source.write_text(fixture.read_text().split("=== FILE: main.dc ===\n", 1)[1].split("\n=== EXPECT-I8086-RUN ===", 1)[0])
            for level in ("-O0", "-O2"):
                obj = directory / f"wrap-{model}{level}.o"
                image = obj.with_suffix(".bin")
                result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level,
                                         "-c", str(source), "-o", str(obj)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
                result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbase=1000:0", str(crt0), str(obj),
                                         "-o", str(image)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
                result = subprocess.run([sys.executable, str(Path(__file__).resolve().parent.parent / "i8086/run.py"), str(image), "--mode",
                                         "real" if model == "small" else "unreal", "--accel", "kvm"], capture_output=True, timeout=60)
                assert result.returncode == 42, result.stderr
                checks += 1
    else:
        print("  NOTE    no /dev/kvm: structural and TCG wrap checks remain enabled")
    string_fixture = Path(__file__).resolve().parent.parent / "cases/i8086/early-string-literal-exec.dcc-test"
    string_source = directory / "early-string.dc"
    string_source.write_text(string_fixture.read_text().split("=== FILE: main.dc ===\n", 1)[1].split("\n=== EXPECT-I8086-RUN ===", 1)[0])
    for model in ("small", "unreal"):
        for level in ("-O0", "-O2"):
            base = ["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbackend", "custom", level]
            obj = directory / f"string-{model}{level}.o"
            listing = obj.with_suffix(".s")
            reassembled = directory / f"string-{model}{level}-nasm.o"
            for flag, output in (("-c", obj), ("-S", listing)):
                result = subprocess.run(base + [flag, str(string_source), "-o", str(output)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
            result = subprocess.run(["timeout", "60", "nasm", "-f", "elf32", str(listing), "-o", str(reassembled)], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            images = []
            for linked in (obj, reassembled):
                image = linked.with_suffix(".bin")
                result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model, "-fbase=1000:0", str(crt0), str(linked),
                                         "-o", str(image)], capture_output=True, text=True)
                assert result.returncode == 0, result.stderr
                images.append(image.read_bytes())
            assert images[0] == images[1], (model, level)
            result = subprocess.run([sys.executable, str(Path(__file__).resolve().parent.parent / "i8086/run.py"), str(obj.with_suffix(".bin")),
                                     "--mode", "real" if model == "small" else "unreal", "--timeout", "30"], capture_output=True, timeout=60)
            assert result.returncode == 0, (model, level, result.stderr)
            checks += 1
    print("  RESULT  i8086 model diagnostics and assembly round trips: %d/%d passed" % (checks, checks))
