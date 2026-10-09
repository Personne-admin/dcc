from pathlib import Path
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
    print("  RESULT  i8086 model diagnostics and assembly round trips: %d/%d passed" % (checks, checks))
