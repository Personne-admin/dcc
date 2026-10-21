#!/usr/bin/env python3
import subprocess
import sys
import tempfile
from pathlib import Path

compiler = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tests/i8086"))
import dos
import mz

startup = (root / "tests/i8086/dos_start.dc").read_text()
checks = 0


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def run(*args, expect=0, contains=None):
    global checks
    result = subprocess.run(["timeout", "120", *map(str, args)], capture_output=True, text=True)
    assert result.returncode == expect, (args, result.returncode, result.stdout, result.stderr)
    if contains is not None:
        assert contains in result.stderr, (args, result.stderr)
    checks += 1
    return result


def check_header(data):
    image = mz.parse(data)
    header = image.header
    file_size = len(data)
    header_size = header["cparhdr"] * 16
    stored = file_size - header_size
    stack_pointer = header["sp"] if header["sp"] else 0x10000
    memory = header["ss"] * 16 + stack_pointer
    assert header["magic"] == 0x5A4D
    assert header["csum"] == 0
    assert header["ovno"] == 0
    assert header["lfarlc"] == 0x1C
    assert header["cs"] == 0
    assert header["cblp"] == file_size % 512
    assert header["cp"] == (file_size + 511) // 512
    assert header["crlc"] == len(image.relocations)
    assert header_size == align_up(28 + 4 * header["crlc"], 16)
    assert header["minalloc"] == header["maxalloc"]
    assert header_size + stored == file_size
    assert stored > 0
    assert 0 < stack_pointer <= 0x10000 and stack_pointer % 2 == 0
    assert header["ip"] < header["ss"] * 16
    assert memory >= stored
    assert header["minalloc"] == (memory + 15) // 16 - (stored + 15) // 16
    sites = image.relocations
    assert tuple(sorted(set(sites))) == sites
    for site in sites:
        assert site + 2 <= stored, (site, stored)
    loaded, entry, stack = image.load(0x3456)
    assert entry == (0x3456, header["ip"])
    assert stack == (0x3456 + header["ss"], stack_pointer)
    for site in sites:
        before = int.from_bytes(image.payload[site:site + 2], "little")
        after = int.from_bytes(loaded[site:site + 2], "little")
        assert after == (before + 0x3456) & 0xFFFF, site
    return image


def build(directory, name, body, flags=(), expect=0, contains=None, use_startup=True):
    source = directory / (name + ".dc")
    source.write_text("module main;\n" + (startup if use_startup else "") + body)
    output = directory / (name + ".exe")
    result = run(compiler, "-target", "i8086-mz", "-fbackend", "custom", "-O2", *flags, source, "-o", output, expect=expect, contains=contains)
    return output, result


def execute(data, expect):
    global checks
    for paragraph in (0x2000, 0x3456, 0x7000):
        result = dos.run(data, "mz", paragraph=paragraph, timeout=20)
        assert result.error is None, (paragraph, result.error)
        assert result.exit_code == expect, (paragraph, result.exit_code, result.stdout, result.stderr)
    checks += 1


with tempfile.TemporaryDirectory(prefix="dcc-i8086-mz-") as directory_name:
    directory = Path(directory_name)

    output, _ = build(directory, "basic", "@nomangle public i32 dcc_main() { return 42; }\n")
    data = output.read_bytes()
    image = check_header(data)
    assert image.header["ip"] == 0
    execute(data, 42)

    output, _ = build(directory, "static_far", "@nomangle public u16 value = 19;\n@nomangle public u16^ pointer = &value;\n"
                                                     "@nomangle public i32 dcc_main() { return *pointer as i32; }\n")
    data = output.read_bytes()
    image = check_header(data)
    execute(data, 19)
    dgroup = image.header["ss"] * 16
    segment_sites = [site for site in image.relocations if site >= dgroup and int.from_bytes(image.payload[site:site + 2], "little") == image.header["ss"]]
    assert segment_sites, image.relocations
    checks += 1

    output, _ = build(directory, "runtime_far", "@nomangle public u16 value = 19;\n"
                                                      "@nomangle public i32 dcc_main() { u16^ pointer = &value; return *pointer as i32; }\n")
    data = output.read_bytes()
    image = check_header(data)
    execute(data, 19)
    assert any(site < image.header["ss"] * 16 for site in image.relocations)
    checks += 1

    literal = 'asm @[intel] { ".data\\n.globl literal_segment\\nliteral_segment:\\n    .word 0xB800, 0x1234\\n" };\n'
    code_segment = 'asm @[intel] { ".data\\n.globl code_segment_word\\ncode_segment_word:\\n    .word __dcc_code_segment\\n" };\n'
    output, _ = build(directory, "literal", literal + "@nomangle public i32 dcc_main() { return 42; }\n")
    data = output.read_bytes()
    image = check_header(data)
    marker = bytes([0x00, 0xB8, 0x34, 0x12])
    position = image.payload.find(marker)
    assert position >= 0
    assert all(not (site <= position < site + 4) for site in image.relocations), image.relocations
    checks += 1

    output, _ = build(directory, "code_segment", code_segment + "@nomangle public i32 dcc_main() { return 42; }\n")
    data = output.read_bytes()
    image = check_header(data)
    execute(data, 42)
    dgroup = image.header["ss"] * 16
    code_sites = [site for site in image.relocations if site >= dgroup and int.from_bytes(image.payload[site:site + 2], "little") == 0]
    assert code_sites, image.relocations
    checks += 1

    paged = 'asm @[intel] { ".data\\n.globl paged_data\\npaged_data:\\n    .space 700\\n" };\n'
    output, _ = build(directory, "paged", paged + "@nomangle public i32 dcc_main() { return 42; }\n")
    data = output.read_bytes()
    image = check_header(data)
    assert len(data) > 512 and image.header["cblp"] == len(data) % 512 and image.header["cp"] == (len(data) + 511) // 512
    execute(data, 42)

    entry_startup = r"""asm @[intel] {
    \\.section .start, "ax", @progbits
    \\.globl _start
    \\jmp short _real_start
    \\_real_start:
    \\mov ax, offset __dcc_dgroup_segment
    \\mov ds, ax
    \\mov es, ax
    \\cld
    \\mov di, offset __bss_start
    \\mov cx, offset __bss_end
    \\sub cx, di
    \\xor al, al
    \\rep stosb
    \\call dcc_main
    \\mov cl, al
    \\mov ax, 0x4c00
    \\mov al, cl
    \\int 0x21
};
"""
    output, _ = build(directory, "entry", entry_startup + "@nomangle public i32 dcc_main() { return 42; }\n",
                      flags=("-e", "_real_start"), use_startup=False)
    data = output.read_bytes()
    image = check_header(data)
    assert image.header["ip"] == 2, image.header
    execute(data, 42)

    bss_startup = r"""asm @[intel] {
    \\.section .start, "ax", @progbits
    \\.globl _start
    \\_start:
    \\mov ax, offset __dcc_dgroup_segment
    \\mov ds, ax
    \\mov es, ax
    \\cld
    \\mov di, offset __bss_start
    \\mov cx, offset __bss_end
    \\sub cx, di
    \\xor al, al
    \\rep stosb
    \\call dcc_main
    \\mov cl, al
    \\mov ax, 0x4c00
    \\mov al, cl
    \\int 0x21
};
"""
    output, _ = build(directory, "bss_only", bss_startup + "@nomangle public u8[256] buffer;\n"
                                                         "@nomangle public i32 dcc_main() { if buffer[0] != 0 || buffer[255] != 0 { return 1; } return 42; }\n",
                      use_startup=False)
    data = output.read_bytes()
    image = check_header(data)
    assert len(image.payload) >= image.header["ss"] * 16, (len(image.payload), image.header)
    execute(data, 42)

    build(directory, "code_overflow", 'asm @[intel] { ".text\\n.space 70000\\n" };\n@nomangle public i32 dcc_main() { return 42; }\n',
          expect=1, contains="MZ code")
    build(directory, "data_overflow", "@nomangle public u8[0x10000] buffer;\n@nomangle public i32 dcc_main() { return 0; }\n",
          expect=1, contains="MZ DGROUP")

    reference_data = dos.assemble(root / "tests/i8086/programs/dos/reference.asm", directory / "reference.exe", {}, "nasm")
    reference_image = mz.parse(reference_data)
    assert tuple(zip(mz.FIELDS, (0x5A4D, 208, 1, 2, 3, 17, 17, 9, 288, 0, 0, 0, 28, 0))) == tuple(reference_image.header.items())
    assert reference_image.relocations == (41, 153)
    for paragraph in (0x2000, 0x3456, 0x7000):
        loaded, entry, stack = reference_image.load(paragraph)
        for site in reference_image.relocations:
            before = int.from_bytes(reference_image.payload[site:site + 2], "little")
            after = int.from_bytes(loaded[site:site + 2], "little")
            assert after == (before + paragraph) & 0xFFFF
    checks += 1

print("  RESULT  i8086 MZ headers and relocations: %d/%d passed" % (checks, checks))
