#!/usr/bin/env python3
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
import measure

BOUNDARIES = sorted(set([0, 1, 2, 3, 7, 255, 256, 65535, 65536, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0x100000001, 0x1ffffffff, 0x7fffffffffffffff, 0x8000000000000000, 0x8000000000000001, 0xffffffffffffffff]) | {1 << bit for bit in range(64)})


def source(old_source, count, seed):
    start = old_source.index('@nomangle @runtime public DivResult __dcc_udivmod64')
    end = old_source.index('\n)dc";', start)
    old = old_source[start:end].replace('@nomangle @runtime public', '@noinline @runtime').replace('__dcc_udivmod64', 'old_unsigned').replace('__dcc_divmod64', 'old_signed').replace('__dcc_i64_trap(0)', 'trap(0)')
    boundaries = BOUNDARIES
    return f'''module main;
public struct DivResult {{ u64 quotient; u64 remainder; }}
@nomangle extern void dcc_putc(u8 value);
@noinline @runtime u32 trap(u32 zero) {{ return 1 / zero; }}
{old}
volatile u32 initial_seed = {seed};
u64[{len(boundaries)}] boundaries = {{{','.join(hex(x) for x in boundaries)}}};
@noinline u32 next(u32* state) {{ *state = *state * 1664525 + 1013904223; return *state; }}
@noinline void output(u64 value) {{
    for u16 shift = 0; shift < 64; shift += 8 {{ dcc_putc((value >> (shift as u64)) as u8); }}
}}
@noinline @runtime bool check(u64 a, u64 b) {{
    if b == 0 {{ return true; }}
    u64 quotient = a / b;
    u64 remainder = a % b;
    DivResult reference = old_unsigned(a, b);
    if quotient != reference.quotient || remainder != reference.remainder {{ return false; }}
    output(quotient); output(remainder);
    if a == 0x8000000000000000 && b == 0xffffffffffffffff {{ output(0); output(0); return true; }}
    i64 signed_quotient = (a as i64) / (b as i64);
    i64 signed_remainder = (a as i64) % (b as i64);
    reference = old_signed(a, b);
    if (signed_quotient as u64) != reference.quotient || (signed_remainder as u64) != reference.remainder {{ return false; }}
    output(signed_quotient as u64); output(signed_remainder as u64);
    return true;
}}
@nomangle public i32 dcc_main() {{
    for usize i = 0; i < {len(boundaries)}; i += 1 {{
        for usize j = 0; j < {len(boundaries)}; j += 1 {{ if !check(boundaries[i], boundaries[j]) {{ return 1; }} }}
    }}
    u32 state = initial_seed;
    for u32 i = 0; i < {count}; i += 1 {{
        u64 a = next(&state) as u64;
        u64 b = next(&state) as u64;
        u32 shape = i & 3;
        if shape != 0 {{ a = (a << 32) | (next(&state) as u64); }}
        if shape >= 2 {{ b = (b << 32) | (next(&state) as u64); }}
        if shape == 3 {{ a = 0 - a; b = 0 - b; }}
        if !check(a, b) {{ return 2; }}
    }}
    return 0;
}}
'''


def main():
    parser = argparse.ArgumentParser(description='Compare optimized i8086 divisions with the old helper and x86-64 hardware results byte-for-byte.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--count', type=int, default=8192)
    parser.add_argument('--seed', type=int, default=0xC0DEF00D)
    parser.add_argument('--old-revision', default='ae692f4')
    parser.add_argument('--run-timeout', type=int, default=180)
    args = parser.parse_args()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    old = measure.run(['git', '-C', measure.ROOT, 'show', args.old_revision + ':compiler/source/dcc.vfs.cppm'])
    dc = directory / 'main.dc'
    dc.write_text(source(old, args.count, args.seed))
    c = directory / 'oracle.c'
    c.write_text('#include <stdio.h>\nint dcc_main(void);\nvoid dcc_putc(unsigned char value) { putchar(value); }\nint main(void) { return dcc_main(); }\n')
    expected = None
    records = []
    for backend in ('llvm', 'custom'):
        obj = directory / (backend + '.o')
        binary = directory / (backend + '.elf')
        measure.run([measure.ROOT / 'build/bin/dcc', '-O2', '-fbackend', backend, '-c', dc, '-o', obj])
        measure.run(['clang', c, obj, '-o', binary])
        result = subprocess.run([binary], capture_output=True, timeout=args.run_timeout)
        assert result.returncode == 0, (backend, result.returncode, result.stderr)
        if expected is None:
            expected = result.stdout
        assert result.stdout == expected, backend
        records.append({'configuration': backend, 'bytes': len(result.stdout), 'sha256': hashlib.sha256(result.stdout).hexdigest()})
    (directory / 'expected.bin').write_bytes(expected)
    crt = directory / 'crt.o'
    measure.run(['nasm', '-f', 'elf32', measure.ROOT / 'tests/i8086/crt0.asm', '-o', crt])
    for model in ('small', 'unreal', 'unreal32'):
        for opt in ('O0', 'O2'):
            stem = directory / (model + '-' + opt)
            obj = stem.with_suffix('.o')
            binary = stem.with_suffix('.bin')
            measure.run([measure.ROOT / 'build/bin/dcc', '-target', 'i8086-binary', '-mcmodel', model, '-fbackend', 'custom', '-' + opt, '-c', dc, '-o', obj])
            measure.run([measure.ROOT / 'build/bin/dcc', '-target', 'i8086-binary', '-mcmodel', model, '-fbase', '1000:0000', crt, obj, '-o', binary])
            serial = stem.with_suffix('.serial')
            command = ['python3', measure.ROOT / 'tests/i8086/run.py', binary, '--mode', 'real' if model == 'small' else 'unreal', '--timeout', str(args.run_timeout), '--serial-out', serial]
            result = subprocess.run(list(map(str, command)), capture_output=True, timeout=args.run_timeout + 10)
            assert result.returncode == 0, (model, opt, result.returncode, result.stderr)
            assert serial.read_bytes() == expected, (model, opt, 'result stream differs from x86-64')
            record = {'configuration': model + '-' + opt, 'bytes': len(expected), 'sha256': hashlib.sha256(expected).hexdigest()}
            records.append(record)
            print(model, opt, 'PASS', len(expected), 'oracle bytes', flush=True)
    report = {'seed': args.seed, 'random_pairs': args.count, 'boundary_pairs': len(BOUNDARIES) ** 2, 'old_revision': args.old_revision, 'new_revision': measure.run(['git', '-C', measure.ROOT, 'rev-parse', 'HEAD']).strip(), 'records': records}
    (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
