from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
runner = Path(__file__).resolve().parent.parent / "i8086" / "run.py"
checks = 0


def run(*args, expect=0, contains=None):
    global checks
    result = subprocess.run(["timeout", "60", *map(str, args)], capture_output=True, text=True)
    assert result.returncode == expect, (args, result.returncode, result.stdout, result.stderr)
    if contains is not None:
        assert contains in result.stderr, (args, result.stderr)
    checks += 1
    return result


def nasm(directory, name, text):
    source = directory / f"{name}.asm"
    source.write_text(text)
    run("nasm", "-f", "elf32", source, "-o", directory / f"{name}.o")
    return directory / f"{name}.o"


with tempfile.TemporaryDirectory(prefix="dcc-i8086-link-") as directory:
    directory = Path(directory)
    i8086 = [compiler, "-target", "i8086-binary", "-fbackend", "custom"]
    crt0 = nasm(directory, "crt0", "bits 16\nsection .start progbits alloc exec\nextern dcc_main\n\tcall dcc_main\n\tmov dx, 0xf4\n\tout dx, al\n.hang:\n\tjmp .hang\n")
    source = directory / "main.dc"
    source.write_text("module m;\n@nomangle public i32 dcc_main() { return 42; }\npublic i16 counter = 3;\n")

    for model, mode in (("small", "real"), ("unreal", "unreal"), ("unreal32", "unreal")):
        for level in ("-O0", "-O2"):
            obj = directory / f"main-{model}{level}.o"
            binary = directory / f"main-{model}{level}.bin"
            run(*i8086, f"-mcmodel={model}", level, "-c", source, "-o", obj)
            run(*i8086, f"-mcmodel={model}", "-fbase=1000:0", crt0, obj, "-o", binary)
            assert binary.read_bytes()[:3] == bytes([0xE8, 0x06, 0x00]), binary.read_bytes()[:8]
            run(sys.executable, runner, "--load", "1000:0000", "--mode", mode, binary, expect=42)

    obj = directory / "main.o"
    run(*i8086, "-c", source, "-o", obj)
    data = nasm(directory, "data", "bits 16\nsection .data\nglobal pointer\npointer:\n\tdw pointer\n")
    linked = directory / "offset.bin"
    run(*i8086, "-fbase=0050:0100", crt0, data, obj, "-o", linked)
    image = linked.read_bytes()
    assert image[:3] == bytes([0xE8, 0x06, 0x00]), image.hex()
    assert any(int.from_bytes(image[f:f + 2], "little") == 0x100 + f for f in range(len(image) - 8, len(image) - 1)), image.hex()
    checks += 1

    single = directory / "single.bin"
    run(*i8086, "-fbase=1000:0", source, "-o", single)
    assert single.read_bytes()[:3] == bytes([0x55, 0x89, 0xE5]), single.read_bytes()[:8]
    checks += 1

    segment_user = nasm(directory, "segment", "bits 16\nsection .text\nglobal segment_of\nextern __dcc_dgroup_segment\nsegment_of:\n\tmov ax, __dcc_dgroup_segment\n\tret\n")
    run(*i8086, crt0, obj, segment_user, "-o", directory / "x.bin", expect=1,
        contains="the image segment is unknown (-fbase=?:0000), but a static far pointer needs it; pass -fbase=SEG:OFF with a known segment")
    run(*i8086, "-fbase=0800:0", crt0, obj, segment_user, "-o", directory / "known.bin")
    assert bytes([0xB8, 0x00, 0x08, 0xC3]) in (directory / "known.bin").read_bytes(), (directory / "known.bin").read_bytes().hex()
    checks += 1

    big_code = nasm(directory, "big", "bits 16\nsection .text\ntimes 0x10001 db 0x90\n")
    for model in ("small", "unreal", "unreal32"):
        run(*i8086, f"-mcmodel={model}", crt0, obj, big_code, "-o", directory / "x.bin", expect=1,
            contains="i8086 code (.start and .text) does not fit below offset 0x10000 of its 64 KiB code segment")

    stack_bss = nasm(directory, "stack", "bits 16\nsection .bss\nresb 0xF000\n")
    run(*i8086, crt0, obj, stack_bss, "-o", directory / "x.bin", expect=1,
        contains="i8086 small model: the image plus the 4096-byte stack reserve (-fstack-reserve) does not fit in one 64 KiB segment")
    run(*i8086, "-fstack-reserve=0", crt0, obj, stack_bss, "-o", directory / "x.bin")

    huge_bss = nasm(directory, "huge", "bits 16\nsection .bss\nresb 0x30000\n")
    run(*i8086, "-mcmodel=small", crt0, obj, huge_bss, "-o", directory / "x.bin", expect=1, contains="i8086 small model")
    for model in ("unreal", "unreal32"):
        run(*i8086, f"-mcmodel={model}", crt0, obj, huge_bss, "-o", directory / "x.bin")

    run(compiler, "-fbase=0:0", "-c", source, "-o", directory / "x.o", expect=1, contains="-fbase applies only to target 'i8086-binary' (target: 'x86_64-elf')")
    run(compiler, "-fstack-reserve=8", "-c", source, "-o", directory / "x.o", expect=1, contains="-fstack-reserve applies only to target 'i8086-binary'")
    run(*i8086, "-fbase=10000:0", crt0, obj, "-o", directory / "x.bin", expect=1,
        contains="invalid -fbase value '10000:0'; expected SEG:OFF in hexadecimal, with SEG '?' if unknown")
    run(*i8086, "-fbase=1000", crt0, obj, "-o", directory / "x.bin", expect=1, contains="invalid -fbase value '1000'")
    run(*i8086, "-fstack-reserve=70000", crt0, obj, "-o", directory / "x.bin", expect=1,
        contains="invalid -fstack-reserve value '70000'; expected a byte count from 0 to 65535")
    run(*i8086, "-flibdcext", "linux", crt0, obj, "-o", directory / "x.bin", expect=1)

print("  RESULT  i8086 flat-binary linking: %d/%d passed" % (checks, checks))
