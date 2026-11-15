from pathlib import Path
import signal
import subprocess
import sys
import tempfile

START = ('asm @[intel, arch("x86")] {\n'
         '    ".text\\n.global _start\\n_start:\\nmov eax, [esp]\\nand esp, -16\\nsub esp, 12\\npush eax\\n'
         'call dcc_main\\nmov ebx, eax\\nmov eax, 1\\nint 0x80\\nud2\\n"\n};\n')

PROGRAM = '''module main;
import helper;
@nomangle u64 g_state = 0x9E3779B97F4A7C15;
u64 next() {
    g_state = g_state ^ (g_state << 13);
    g_state = g_state ^ (g_state >> 7);
    g_state = g_state ^ (g_state << 17);
    return g_state;
}
i32 check_unsigned(u64 n, u64 d) {
    u64 q = n / d;
    u64 r = n % d;
    if r >= d { return 1; }
    if q * d + r != n { return 2; }
    return 0;
}
i32 check_signed(i64 n, i64 d) {
    i64 q = n / d;
    i64 r = n % d;
    if q * d + r != n { return 3; }
    if r != 0 && (r < 0) != (n < 0) { return 4; }
    i64 magnitude = if r < 0 { 0 - r } else { r };
    i64 limit = if d < 0 { 0 - d } else { d };
    if magnitude >= limit { return 5; }
    return 0;
}
@nomangle public i32 dcc_main(i32 argc) {
    if sizeof(usize) != 4 || sizeof(void*) != 4 { return 10; }
    if helper::scaled(argc as u64 + 99) != 10 { return 11; }
    for u32 i = 0; i < 4000; i++ {
        u64 n = next();
        u64 d = next() >> ((i % 64) as u64);
        if d == 0 { d = argc as u64; }
        i32 status = check_unsigned(n, d);
        if status != 0 { return status; }
        i64 sd = d as i64;
        if sd == -1 && n == 0x8000000000000000 { sd = 3; }
        status = check_signed(n as i64, sd);
        if status != 0 { return status; }
    }
    u64 accumulate = argc as u64;
    accumulate /= 1;
    accumulate %= 0x100000001;
    if accumulate != 1 { return 12; }
    return 42;
}
'''

HELPER = '''module helper;
public u64 scaled(u64 value) { return value / 10; }
'''

ZERO = '''module main;
@nomangle public i32 dcc_main(i32 argc) {
    u64 zero = argc as u64 - 1;
    return (0x123456789 / zero) as i32;
}
'''


def run(argv, expect=0, contains=()):
    result = subprocess.run(['timeout', '60', *map(str, argv)], capture_output=True, text=True, timeout=65)
    output = result.stdout + result.stderr
    assert result.returncode == expect, (argv, result.returncode, output)
    for text in contains:
        assert text in output, (argv, text, output)
    return output


def inspect(exe):
    header = run(['readelf', '-h', exe])
    assert 'ELF32' in header and 'Intel 80386' in header and 'EXEC (Executable file)' in header, header
    entry = int(next(line for line in header.splitlines() if 'Entry point address' in line).split()[-1], 16)
    symbols = run(['nm', exe])
    start = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' _start'))
    assert entry == start, (hex(entry), hex(start))
    assert 'INTERP' not in run(['readelf', '-l', exe])
    assert ' U ' not in symbols, symbols


def execute(exe, expect):
    result = subprocess.run([str(exe)], capture_output=True, timeout=60)
    assert result.returncode == expect, (exe, result.returncode)


def main():
    dcc = Path(sys.argv[1]).resolve()
    checks = 0
    with tempfile.TemporaryDirectory(prefix='dcc x86 exe ') as raw:
        root = Path(raw)
        main_source = root / 'main.dc'
        main_source.write_text(PROGRAM + START)
        (root / 'helper.dc').write_text(HELPER)
        zero = root / 'zero.dc'
        zero.write_text(ZERO + START)
        base = [dcc, '-target', 'x86-elf', '-I', root]
        link = [dcc, '-target', 'x86-elf']
        for opt in ('-O0', '-O2'):
            for mode in ('whole', 'whole-object', 'objects'):
                exe = root / f'program {opt} {mode}'
                main_object, helper_object = root / 'main.o', root / 'helper.o'
                if mode == 'objects':
                    run([*base, opt, '-c', main_source, '-o', main_object])
                    run([*base, opt, '-c', root / 'helper.dc', '-o', helper_object])
                    run([*link, main_object, helper_object, '-o', exe])
                elif mode == 'whole-object':
                    run([*base, opt, '-fwhole-program', '-c', main_source, '-o', main_object])
                    run([*link, main_object, '-o', exe])
                else:
                    run([*base, opt, '-fwhole-program', main_source, '-o', exe])
                inspect(exe)
                execute(exe, 42)
                checks += 1
                print('  PASS x86-elf executable', opt, mode, flush=True)
            trap = root / f'zero {opt}'
            run([*base, opt, zero, '-o', trap])
            execute(trap, -signal.SIGILL)
            checks += 1

        x86_64_object = root / 'x86_64.o'
        run([dcc, '-target', 'x86_64-elf', '-I', root, '-c', root / 'helper.dc', '-o', x86_64_object])
        run([*link, x86_64_object, '-o', root / 'mixed'], expect=1, contains=("not an i386 ELF object for target 'x86-elf'",))
        run([*base, '-fbackend', 'custom', '-c', root / 'helper.dc', '-o', root / 'custom.o'], expect=1,
            contains=("custom backend: target 'x86-elf' has no native code generator",))
        assert not (root / 'custom.o').exists()
        run([dcc, '-target', 'x86-coff', root / 'helper.dc', '-o', root / 'pe32.exe'], expect=1,
            contains=('PE executable emission requires x86_64',))
        assert not (root / 'mixed').exists() and not (root / 'pe32.exe').exists()
        checks += 4
    print(f'  RESULT  {checks}/{checks} x86 executable checks passed')


if __name__ == '__main__':
    main()
