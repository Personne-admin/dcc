from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from i8086 import freedos

compiler = Path(sys.argv[1]).resolve()
dos_start = (ROOT / "i8086" / "dos_start.dc").read_text()
com_start = (ROOT / "i8086" / "dos_com_start.dc").read_text()
checks = 0


def compile_source(directory, name, source, kind, opt):
    path = directory / f"{name}.dc"
    startup = dos_start if kind == "EXE" else com_start
    path.write_text(source + "\n" + startup)
    output = directory / f"{name}.{ 'exe' if kind == 'EXE' else 'com' }"
    if kind == "EXE":
        args = [compiler, "-target", "i8086-mz", "-fbackend", "custom", "-mcmodel=small", "-e", "_start",
                "-fstack-reserve=4096", opt, path, "-o", output]
    else:
        args = [compiler, "-target", "i8086-binary", "-fbackend", "custom", "-mcmodel=small",
                "-fbase=?:0x100", "-e", "_start", opt, path, "-o", output]
    result = subprocess.run(["timeout", "60", *map(str, args)], capture_output=True, text=True)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return output


def check_run(program, kind, expected_exit, stdout=None, mode="", opt="-O0"):
    global checks
    result = freedos.run(program, kind, mode=mode)
    assert result.ok, (kind, result.status_kind, result.status_code, result.stdout, result.stderr)
    assert result.status_code == expected_exit, (kind, result.status_code, expected_exit)
    if stdout is not None:
        assert result.stdout == stdout, (kind, result.stdout, stdout)
    checks += 1
    return result


putc = r'''asm @[intel] { ".text\n.globl putc\nputc:\n    push bp\n    mov bp, sp\n    push bx\n    push cx\n    push dx\n    mov al, byte ptr [bp + 4]\n    mov byte ptr putc_buf, al\n    mov ax, 0x4000\n    mov bx, 1\n    mov cx, 1\n    mov dx, offset putc_buf\n    int 0x21\n    pop dx\n    pop cx\n    pop bx\n    pop bp\n    ret\n.data\nputc_buf:\n    .byte 0\n" };
@nomangle extern void putc(u8 c);
'''

psp = r'''asm @[intel] { ".text\n.globl psp_seg\npsp_seg:\n    mov ax, 0x6200\n    int 0x21\n    mov ax, bx\n    ret\n.globl tail_ok\ntail_ok:\n    push ds\n    mov ax, 0x6200\n    int 0x21\n    mov ds, bx\n    cmp byte ptr [0x80], 4\n    jne bad\n    cmp byte ptr [0x81], ' '\n    jne bad\n    cmp byte ptr [0x82], 'A'\n    jne bad\n    cmp byte ptr [0x83], 'R'\n    jne bad\n    cmp byte ptr [0x84], 'G'\n    jne bad\n    pop ds\n    mov ax, 1\n    ret\nbad:\n    pop ds\n    xor ax, ax\n    ret\n" };
@nomangle extern u16 psp_seg();
@nomangle extern u16 tail_ok();
'''

env = r'''asm @[intel] { ".text\n.globl env_os\nenv_os:\n    push ds\n    mov ax, 0x6200\n    int 0x21\n    mov ds, bx\n    mov ax, [0x2C]\n    mov ds, ax\n    xor ax, ax\n    mov al, [0]\n    pop ds\n    ret\n" };
@nomangle extern u16 env_os();
'''

hexout = r'''@nomangle void put_hex(u16 value) {
    for i32 i = 12; i >= 0; i -= 4 {
        u16 digit = (value >> i as u16) & 0xF;
        putc((if digit < 10 { 48 + digit } else { 55 + digit }) as u8);
    }
}
'''

with tempfile.TemporaryDirectory(prefix="dcc-freedos-tests-") as directory:
    directory = Path(directory)
    source = "module main;\n" + putc + "@nomangle public i32 dcc_main() { putc('A' as u8); putc('B' as u8); return 42; }\n"
    mz = compile_source(directory, "mz", source, "EXE", "-O0")
    check_run(mz, "EXE", 42, b"AB")
    mz2 = compile_source(directory, "mz2", source, "EXE", "-O2")
    check_run(mz2, "EXE", 42, b"AB")
    com = compile_source(directory, "com", source, "COM", "-O0")
    check_run(com, "COM", 42, b"AB")

    tail_source = "module main;\n" + psp + "@nomangle public i32 dcc_main() { if tail_ok() == 0 { return 1; } return 42; }\n"
    check_run(compile_source(directory, "tail_mz", tail_source, "EXE", "-O0"), "EXE", 42)
    check_run(compile_source(directory, "tail_com", tail_source, "COM", "-O0"), "COM", 42)

    env_source = "module main;\n" + env + "@nomangle public i32 dcc_main() { if env_os() != 67 { return 1; } return 42; }\n"
    check_run(compile_source(directory, "env_mz", env_source, "EXE", "-O0"), "EXE", 42)

    segment_source = "module main;\n" + putc + psp + hexout + "@nomangle public i32 dcc_main() { put_hex(psp_seg()); return 42; }\n"
    segment = compile_source(directory, "segment", segment_source, "EXE", "-O0")
    normal = check_run(segment, "EXE", 42)
    big = check_run(segment, "EXE", 42, mode="BIG")
    normal_seg = int(normal.stdout.decode("ascii"), 16)
    big_seg = int(big.stdout.decode("ascii"), 16)
    assert normal_seg != big_seg, (normal_seg, big_seg)
    assert normal_seg > 0x100 and big_seg > 0x100, (normal_seg, big_seg)
    checks += 1

    low = freedos.run(segment, "EXE", mode="LOW")
    assert low.status_kind == "E", (low.status_kind, low.status_code)
    assert low.status_code != 0, low.status_code
    checks += 1

    math = "module main;\n@nomangle public i32 dcc_main() { u32 x = 0x12345678; if x / 3 * 3 + x % 3 != x { return 1; } return 42; }\n"
    check_run(compile_source(directory, "math", math, "EXE", "-O2"), "EXE", 42)

print(f"  RESULT  FreeDOS realism: {checks}/{checks} passed")