from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
oracle = compiler.parent / "tests" / "i8086_link_oracle"
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


def link(model, *inputs, output, expect=0, contains=None):
    return run(oracle, f"-mcmodel={model}", *inputs, "-o", output, expect=expect, contains=contains)


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
            link(model, crt0, obj, output=binary)
            assert binary.read_bytes()[:3] == bytes([0xE8, 0x06, 0x00]), binary.read_bytes()[:8]
            run(sys.executable, runner, "--load", "1000:0000", "--mode", mode, binary, expect=42)

    obj = directory / "main.o"
    run(*i8086, "-c", source, "-o", obj)
    objects = {}
    for model in ("small", "unreal", "unreal32"):
        objects[model] = directory / f"model-{model}.o"
        run(*i8086, f"-mcmodel={model}", "-c", source, "-o", objects[model])

    for link_model in ("small", "unreal", "unreal32"):
        script = directory / f"{link_model}.ld"
        script.write_text(run(*i8086, f"-mcmodel={link_model}", "--print-linker-script").stdout)
        for object_model, object_path in objects.items():
            marker = f"__dcc_i8086_model_{object_model}"
            if object_model == link_model:
                link(link_model, crt0, object_path, output=directory / "x.bin")
                run("ld.lld", "-m", "elf_i386", "-T", script, "-o", directory / "raw.bin", crt0, object_path)
                continue
            link(link_model, crt0, object_path, output=directory / "x.bin", expect=1,
                 contains=f"i8086 code model mismatch: an object needs {marker}, but this link is for -mcmodel={link_model} and provides "
                          f"__dcc_i8086_model_{link_model}")
            raw = run("ld.lld", "-m", "elf_i386", "-T", script, "-o", directory / "raw.bin", crt0, object_path, expect=1)
            assert f"undefined symbol: {marker}" in raw.stderr, raw.stderr
    link("default", crt0, objects["small"], output=directory / "x.bin")
    link("unreal", crt0, obj, output=directory / "x.bin", expect=1, contains="needs __dcc_i8086_model_small")
    run(compiler, "--print-linker-script", expect=1, contains="--print-linker-script applies only to target 'i8086-binary'")
    script_text = run(*i8086, "-mcmodel=unreal", "-fbase=0800:0100", "--print-linker-script").stdout
    assert "__dcc_i8086_model_unreal = 0;" in script_text and ". = 0x100;" in script_text, script_text
    assert "__dcc_dgroup_segment = 0x800;" in script_text, script_text
    data = nasm(directory, "data", "bits 16\nsection .data\nglobal pointer\npointer:\n\tdw pointer\n")
    linked = directory / "offset.bin"
    run(oracle, "-mcmodel=small", "-fbase=0050:0100", crt0, data, obj, "-o", linked)
    image = linked.read_bytes()
    assert image[:3] == bytes([0xE8, 0x06, 0x00]), image.hex()
    assert any(int.from_bytes(image[f:f + 2], "little") == 0x100 + f for f in range(len(image) - 8, len(image) - 1)), image.hex()
    checks += 1

    segment_user = nasm(directory, "segment", "bits 16\nsection .text\nglobal segment_of\nextern __dcc_dgroup_segment\nsegment_of:\n\tmov ax, __dcc_dgroup_segment\n\tret\n")
    link("small", crt0, obj, segment_user, output=directory / "x.bin", expect=1,
         contains="the image segment is unknown (-fbase=?:0000), but a static far pointer needs it; pass -fbase=SEG:OFF with a known segment")
    run(oracle, "-mcmodel=small", "-fbase=0800:0", crt0, obj, segment_user, "-o", directory / "known.bin")
    assert bytes([0xB8, 0x00, 0x08, 0xC3]) in (directory / "known.bin").read_bytes(), (directory / "known.bin").read_bytes().hex()
    checks += 1

    far_source = directory / "far-global.dc"
    far_source.write_text("module far_global;\n@nomangle u16 value = 19;\n@nomangle u16^ pointer = &value;\n"
                          "@nomangle public i32 dcc_main() { if *pointer != 19 { return 1; } return 42; }\n")
    for model, mode in (("small", "real"), ("unreal", "unreal"), ("unreal32", "unreal")):
        for level in ("-O0", "-O2"):
            far_object = directory / f"far-{model}-{level}.o"
            run(*i8086, f"-mcmodel={model}", level, "-c", far_source, "-o", far_object)
            link(model, crt0, far_object, output=directory / "unknown-far.bin", expect=1,
                 contains="the image segment is unknown (-fbase=?:0000), but a static far pointer needs it; pass -fbase=SEG:OFF with a known segment")
            far_image = directory / f"far-{model}-{level}.bin"
            run(oracle, f"-mcmodel={model}", "-fbase=1000:0", crt0, far_object, "-o", far_image)
            run(sys.executable, runner, "--load", "1000:0000", "--mode", mode, far_image, expect=42)

    big_code = nasm(directory, "big", "bits 16\nsection .text\ntimes 0x10001 db 0x90\n")
    for model in ("small", "unreal", "unreal32"):
        link(model, crt0, objects[model], big_code, output=directory / "x.bin", expect=1,
             contains="i8086 code (.start and .text) does not fit below offset 0x10000 of its 64 KiB code segment")

    stack_bss = nasm(directory, "stack", "bits 16\nsection .bss\nresb 0xF000\n")
    link("small", crt0, obj, stack_bss, output=directory / "x.bin", expect=1,
         contains="i8086 small model: the image plus the 4096-byte stack reserve (-fstack-reserve) does not fit in one 64 KiB segment")
    run(oracle, "-mcmodel=small", "-fstack-reserve=0", crt0, obj, stack_bss, "-o", directory / "x.bin")

    for model in ("unreal", "unreal32"):
        script = run(*i8086, f"-mcmodel={model}", "--print-linker-script").stdout
        for section in (".rodata", ".data", ".bss"):
            assert f"SIZEOF({section}) > 0xF000 ? 0x10000" in script, script
            checks += 1
    large_data = nasm(directory, "large-data", "bits 16\nsection .data\ntimes 0x20001 db 0x5a\n")
    link("small", crt0, objects["small"], large_data, output=directory / "large-small.bin", expect=1, contains="i8086 small model")
    for model in ("unreal", "unreal32"):
        image = directory / f"large-data-{model}.bin"
        map_file = directory / f"large-data-{model}.map"
        run(oracle, f"-mcmodel={model}", "-fbase=1000:0", crt0, objects[model], large_data, "-Wl,-Map=" + str(map_file), "-o", image)
        payload = image.read_bytes()
        input_line = next(line for line in map_file.read_text().splitlines() if "large-data.o:(.data)" in line)
        fields = input_line.split()
        address, size = int(fields[0], 16), int(fields[2], 16)
        assert address >= 0x10000 and size == 0x20001, input_line
        assert payload[address:address + size] == bytes([0x5a]) * size
        checks += 1
    huge_bss = nasm(directory, "huge", "bits 16\nsection .bss\nresb 0x30000\n")
    link("small", crt0, objects["small"], huge_bss, output=directory / "x.bin", expect=1, contains="i8086 small model")
    for model in ("unreal", "unreal32"):
        link(model, crt0, objects[model], huge_bss, output=directory / "x.bin")

    sections = directory / "sections.dc"
    sections.write_text("module s;\n@section(\".rodata.table\")\npublic const u16 table = 0x1234;\n@section(\".data.extra\")\npublic u16 extra = 0x5678;\n"
                        "@nomangle public i32 dcc_main() { return 7; }\n")
    run(*i8086, "-c", sections, "-o", directory / "sections.o")
    placed = directory / "sections.bin"
    run(oracle, "-mcmodel=small", "-fbase=1000:0", crt0, directory / "sections.o", "-o", placed)
    assert bytes([0x34, 0x12]) in placed.read_bytes() and bytes([0x78, 0x56]) in placed.read_bytes(), placed.read_bytes().hex()
    run(sys.executable, runner, "--load", "1000:0000", "--mode", "real", placed, expect=7)

    custom = directory / "custom.dc"
    custom.write_text("module c;\n@section(\".vectors\")\npublic u16 vector = 1;\n@nomangle public i32 dcc_main() { return 7; }\n")
    run(*i8086, "-c", custom, "-o", directory / "custom.o")
    link("small", crt0, directory / "custom.o", output=directory / "x.bin", expect=1,
         contains="allocated section .vectors (" + str(directory / "custom.o") + ":(.vectors)) is not placed by the i8086 linker script")

    orphans = nasm(directory, "orphans", "bits 16\nsection .init progbits alloc exec\n\tret\nsection .mysec progbits alloc write\n\tdb 2\n"
                   "section .myinfo progbits noalloc\n\tdb 3\nsection .text.hot progbits alloc exec\n\tret\nsection .rodata.str progbits alloc\n\tdb 1\n")
    result = link("small", crt0, obj, orphans, output=directory / "y.bin", expect=1)
    for name in (".init", ".mysec"):
        assert f"allocated section {name} ({orphans}:({name})) is not placed by the i8086 linker script" in result.stderr, result.stderr
    for name in (".myinfo", ".text.hot", ".rodata.str"):
        assert f"section {name} " not in result.stderr, result.stderr
    assert not (directory / "y.bin").exists(), "a rejected link must not produce an image"
    fine = nasm(directory, "fine", "bits 16\nsection .myinfo progbits noalloc\n\tdb 3\nsection .text.hot progbits alloc exec\n\tret\nsection .rodata.str progbits alloc\n\tdb 1\n")
    run(oracle, "-mcmodel=small", "-fbase=1000:0", crt0, obj, fine, "-o", directory / "fine.bin")
    run(sys.executable, runner, "--load", "1000:0000", "--mode", "real", directory / "fine.bin", expect=42)
    custom_script = directory / "custom.ld"
    custom_script.write_text(run(*i8086, "--print-linker-script").stdout.replace(".data : {", ".data : { *(.mysec .init) "))
    run(oracle, "-mcmodel=small", "-Wl,-T," + str(custom_script), crt0, obj, orphans, "-o", directory / "z.bin")

    run(compiler, "-fbase=0:0", "-c", source, "-o", directory / "x.o", expect=1, contains="-fbase applies only to target 'i8086-binary' (target: 'x86_64-elf')")
    run(compiler, "-fstack-reserve=8", "-c", source, "-o", directory / "x.o", expect=1, contains="-fstack-reserve applies only to target 'i8086-binary'")
    run(*i8086, "-fbase=10000:0", source, "-o", directory / "x.bin", expect=1,
        contains="invalid -fbase value '10000:0'; expected SEG:OFF in hexadecimal, with SEG '?' if unknown")
    run(*i8086, "-fbase=1000", source, "-o", directory / "x.bin", expect=1, contains="invalid -fbase value '1000'")
    run(*i8086, "-fstack-reserve=70000", source, "-o", directory / "x.bin", expect=1,
        contains="invalid -fstack-reserve value '70000'; expected a byte count from 0 to 65535")
    run(*i8086, "-flibdcext", "linux", source, "-o", directory / "x.bin", expect=1, contains="not available for target 'i8086-binary'")

print("  RESULT  i8086 flat-binary linking (oracle): %d/%d passed" % (checks, checks))
