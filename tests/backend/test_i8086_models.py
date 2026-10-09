from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def section(path, name):
    data = path.read_bytes()
    assert data[:4] == b"\x7fELF" and data[4] == 1 and struct.unpack_from("<H", data, 18)[0] == 3, path
    shoff = struct.unpack_from("<I", data, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    headers = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    strtab = headers[shstrndx]
    for header in headers:
        end = data.index(b"\0", strtab[4] + header[0])
        if data[strtab[4] + header[0]:end].decode() == name:
            return data[header[4]:header[4] + header[5]]
    return None


compiler = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="dcc-i8086-models-") as directory:
    directory = Path(directory)
    source = directory / "main.dc"
    source.write_text("module m;\n@nomangle public i32 dcc_main() { return 0x12345; }\npublic u16 value() { return 7; }\npublic bool flag() { return true; }\n"
                      "public void nothing() {}\npublic i16 counter = 3;\n")
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
            for name in (".text", ".data", ".dcc.i8086.model"):
                assert section(obj, name) is not None and section(obj, name) == section(reassembled, name), (model, level, name)
            checks += 1
    print("  RESULT  i8086 model diagnostics and assembly round trips: %d/%d passed" % (checks, checks))
