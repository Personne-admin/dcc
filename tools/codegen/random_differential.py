#!/usr/bin/env python3
import argparse
import concurrent.futures
import hashlib
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/i8086'))
from run import run as run_guest

FIXED_SEEDS = (0, 1, 17, 42, 65537, 0xC0DEF00D)
CONFIGS = ('llvm-O0', 'custom-O0', 'custom-O2', 'small-O0', 'small-O2',
           'unreal-O0', 'unreal-O2', 'unreal32-O0', 'unreal32-O2')


def generate(seed, keep=None):
    rng = random.Random(seed)
    functions = []
    blocks = []
    for typ in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64'):
        a, b, shift, trips = rng.randint(1, 7), rng.randint(1, 5), rng.randrange(3), rng.randint(1, 7)
        op = rng.choice(('+', '-', '*', '/', '%'))
        relation = rng.choice(('<', '<=', '>', '>=', '==', '!='))
        functions.append(f'''@noinline @runtime u32 arithmetic_{typ}({typ} x, {typ} y) {{
    {typ} value = x {op} y;
    {typ} bits = ((x & 3) << {shift}) ^ y;
    if x {relation} y {{ value = value + 1; }} else {{ value = value - 1; }}
    for u16 i = 0; i < {trips}; i += 1 {{
        if (i & 1) == 0 {{ value += 1; }} else {{ value -= 1; }}
    }}
    return (value as u32) ^ (bits as u32);
}}''')
        blocks.append(f'mix(&checksum, arithmetic_{typ}(((input & 3) + {a}) as {typ}, {b} as {typ}));')
    high = rng.randrange(1, 0x3fffffff)
    divisor = rng.choice((3, 7, 0xffffffff, 0x100000001))
    functions.append(f'''@noinline @runtime u32 wide(u32 x) {{
    u64 value = (({high} as u64) << 32) | (x as u64);
    u64 q = value / {divisor};
    u64 r = value % {divisor};
    i64 negative = 0 - (value as i64);
    i64 sq = negative / ({divisor} as i64);
    i64 sr = negative % ({divisor} as i64);
    return (q as u32) ^ ((q >> 32) as u32) ^ (r as u32) ^ (sq as u32) ^ (sr as u32);
}}''')
    blocks.append('mix(&checksum, wide(input));')
    delta = rng.randint(1, 13)
    functions.append(f'''@noinline @runtime void poke(u16* p, u16 delta) {{ *p += delta; ++*p; }}
@noinline @runtime u32 memory(u32 x) {{
    Record record = {{ tag = (x & 31) as u8, small = 17, word = x, wide = (x as u64) + 0x100000001 }};
    u16[8] values = {{1,2,3,4,5,6,7,8}};
    for usize i = 0; i < 8; i += 1 {{ values[i] += ((x & 7) as u16); }}
    []u16 view = values[1..7];
    for usize i = 0; i < view.len; i += 1 {{ poke(&view[i], {delta}); }}
    u16 local = 19;
    poke(&local, {delta});
    poke(&global_word, {delta});
    poke(&record.small, {delta});
    u32 result = (local as u32) + (global_word as u32) + (record.small as u32);
    for usize i = 0; i < 8; i += 1 {{ result += values[i] as u32; }}
    result ^= record.word ^ (record.tag as u32) ^ (record.wide as u32) ^ ((record.wide >> 32) as u32);
    return result;
}}
@noinline @runtime u32 one(u32 x) {{ return x ^ 0x531; }}
@noinline @runtime u32 three(u8 a, i16 b, u64 c) {{ return (a as u32) + (b as u32) + (c as u32) + ((c >> 32) as u32); }}
@noinline @runtime u32 many(u8 a, u16 b, u32 c, u64 d, i64 e, Record f, []u16 g, u16* h, u32 i) {{
    poke(h, 3);
    return (a as u32) + (b as u32) + c + (d as u32) + ((d >> 32) as u32) + (e as u32) +
        (f.tag as u32) + (f.small as u32) + f.word + (f.wide as u32) + (g[1] as u32) + (*h as u32) + i;
}}
@noinline @runtime u32 calls(u32 x) {{
    u16 local = 21;
    u16[3] values = {{7,9,11}};
    Record r = {{tag = 3, small = 5, word = x, wide = (x as u64) + 0x100000003}};
    u32 result = one(x) ^ three(7, -11, (x as u64) + 0x100000001);
    result ^= many(2, 4, x, (x as u64) + 0x100000005, -19, r, values[0..3], &local, 13);
    return result ^ (local as u32);
}}''')
    blocks.extend(('mix(&checksum, memory(input));', 'mix(&checksum, calls(input));'))
    negative = -128 + (seed & 63)
    functions.append(f'''@noinline @runtime u32 signed_casts(i8 x) {{
    i16 wide = x as i16;
    u16 unsigned_wide = x as u16;
    return ((wide == {negative}) as u32) | (((unsigned_wide == {negative & 65535}) as u32) << 1) |
        (((wide < {negative + 1}) as u32) << 2) | (((unsigned_wide > 65000) as u32) << 3) |
        ((((x as i16) == {negative}) as u32) << 4) | ((((x as u16) == {negative & 65535}) as u32) << 5);
}}
@noinline @runtime void save_globals(Record record, []u16 view) {{ global_record = record; global_view = view; }}
@noinline @runtime u32 aggregate_globals(u32 x) {{
    Record record = {{tag = 7, small = 11, word = x, wide = (x as u64) + 0x100000003}};
    u16[3] values = {{13,17,19}};
    save_globals(record, values[0..3]);
    if global_view.len != 3 {{ return 0xbad; }}
    return global_record.word ^ (global_record.small as u32) ^ (global_record.tag as u32) ^
        (global_record.wide as u32) ^ ((global_record.wide >> 32) as u32) ^ (global_view[2] as u32);
}}
@noinline @runtime u32 destructure(u32 x) {{
    Triple value = {{a = x & 255, b = (x & 255) + 1, c = (x & 255) + 2}};
    return match value {{ Triple {{a, b, c}} => a ^ (b << 8) ^ (c << 16), }};
}}
@noinline @runtime Small make_small(u32 x) {{ return Small {{first = x, second = (x & 255) as u16}}; }}
@nomangle @noinline @runtime public Small c_small(u32 x) {{ return make_small(x); }}
@noinline @runtime u32 scalar_shapes(u32 x, bool choose) {{
    Small value = make_small(x);
    u16[4] fixed = {{3,5,7,11}};
    u16[4] data = {{13,17,19,23}};
    []u16 view = data[0..4];
    for u16 i = 0; i < 3; i += 1 {{
        if choose {{ value.first += 3; fixed[1] += 2; view = data[1..3]; }}
        else {{ value.second += 1; fixed[2] += 3; view = data[0..2]; }}
    }}
    Small other = c_small(x + 1);
    return value.first ^ (value.second as u32) ^ (fixed[0] as u32) ^ (fixed[1] as u32) ^
        (fixed[2] as u32) ^ (fixed[3] as u32) ^ (view.len as u32) ^ (view[0] as u32) ^ other.first ^ (other.second as u32);
}}''')
    blocks.extend(('mix(&checksum, signed_casts(negative_input));', 'mix(&checksum, aggregate_globals(input));',
                   'mix(&checksum, scalar_shapes(input, (input & 1) != 0));', 'mix(&checksum, destructure(input));'))
    header = f'''module main;
struct Record {{ u8 tag; u16 small; u32 word; u64 wide; }}
public struct Small {{ u32 first; u16 second; }}
struct Triple {{ u32 a; u32 b; u32 c; }}
Record global_record;
[]u16 global_view;
volatile i8 negative_input = {negative};
volatile u32 initial = {seed & 0xffffffff};
u16 global_word = 23;
const char[17] digits = "0123456789abcdef";
@nomangle extern void dcc_putc(u8 value);
@noinline @runtime void mix(u32* state, u32 value) {{ *state = (*state ^ value) * 1664525 + 1013904223; }}
'''
    chosen = range(len(blocks)) if keep is None else keep
    body = '\n    '.join(blocks[i] for i in chosen)
    return header + '\n'.join(functions) + f'''
@nomangle public i32 dcc_main() {{
    u32 input = initial;
    u32 checksum = 0x9e3779b9;
    {body}
    for u32 i = 0; i < 8; i += 1 {{ dcc_putc(digits[((checksum >> ((7 - i) * 4)) & 15) as usize] as u8); }}
    dcc_putc(10);
    return 0;
}}
''', len(blocks)


def command(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(f'{" ".join(map(str, args))}\n{result.stderr.decode(errors="replace")}')
    return result


def execute(config, directory, dcc, crt, native):
    backend, opt = config.rsplit('-', 1)
    obj = directory / (config + '.o')
    if backend in ('llvm', 'custom'):
        command([dcc, '-fbackend', backend, '-' + opt, '-c', directory / 'main.dc', '-o', obj])
        binary = directory / (config + '.elf')
        command(['clang', native, obj, '-o', binary])
        result = subprocess.run([binary], capture_output=True, timeout=10)
        return dict(exit=result.returncode, output=result.stdout.hex(), stderr=result.stderr.decode(errors='replace'))
    binary = directory / (config + '.bin')
    command([dcc, '-target', 'i8086-binary', '-mcmodel', backend, '-fbackend', 'custom', '-' + opt,
             '-c', directory / 'main.dc', '-o', obj])
    command([dcc, '-target', 'i8086-binary', '-mcmodel', backend, '-fbase', '1000:0000', crt, obj, '-o', binary])
    result = run_guest(binary.read_bytes(), mode='real' if backend == 'small' else 'unreal', timeout=10,
                       workdir=directory)
    return dict(exit=result.exit_code, output=result.serial.hex(), stderr=result.error or '', timeout=result.timed_out)


def check_seed(seed, output, dcc, crt, native, minimize):
    directory = output / f'seed-{seed}'
    directory.mkdir()
    source, count = generate(seed)
    (directory / 'main.dc').write_text(source)
    records = {}
    for config in CONFIGS:
        try:
            records[config] = execute(config, directory, dcc, crt, native)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            records[config] = dict(error=str(error))
    reference = records[CONFIGS[0]]
    valid = reference.get('exit') == 0 and len(reference.get('output', '')) == 18
    failures = [c for c in CONFIGS[1:] if not valid or records[c].get('exit') != 0 or
                records[c].get('output') != reference.get('output') or records[c].get('stderr') or records[c].get('timeout')]
    minimized = None
    if failures and valid and minimize:
        target = failures[0]
        keep = list(range(count))
        trial = directory / 'minimize'
        trial.mkdir()
        for block in range(count):
            candidate = [i for i in keep if i != block]
            text, _ = generate(seed, candidate)
            (trial / 'main.dc').write_text(text)
            try:
                a = execute(CONFIGS[0], trial, dcc, crt, native)
                b = execute(target, trial, dcc, crt, native)
                if a.get('exit') == 0 and (b.get('exit') != 0 or a.get('output') != b.get('output')):
                    keep = candidate
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
        text, _ = generate(seed, keep)
        (directory / 'minimized.dc').write_text(text)
        minimized = dict(configuration=target, retained_blocks=keep)
    report = dict(seed=seed, records=records, failures=failures, reference_valid=valid, minimized=minimized)
    (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'seed {seed}: ' + ('PASS' if valid and not failures else 'FAIL ' + ', '.join(failures)), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description='Seeded cross-backend integer/memory/ABI execution differential.')
    parser.add_argument('--dcc', type=Path, default=ROOT / 'build/bin/dcc')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--seeds', nargs='+', type=lambda s: int(s, 0))
    parser.add_argument('--count', type=int, help='consecutive seeds beginning at --start; default uses the fixed suite')
    parser.add_argument('--start', type=int, default=0)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--minimize', action='store_true', help='greedily remove checksum blocks from failing programs')
    args = parser.parse_args()
    seeds = args.seeds if args.seeds is not None else list(range(args.start, args.start + args.count)) if args.count else FIXED_SEEDS
    if len(set(seeds)) != len(seeds) or args.jobs < 1:
        parser.error('seeds must be unique and jobs positive')
    temporary = tempfile.TemporaryDirectory(prefix='dcc-random-differential-') if args.output is None else None
    output = (args.output or Path(temporary.name)).resolve()
    output.mkdir(parents=True, exist_ok=True)
    native = output / 'native.c'
    native.write_text('#include <stdio.h>\nint dcc_main(void);\nvoid dcc_putc(unsigned char x) { putchar(x); }\nint main(void) { return dcc_main(); }\n')
    crt = output / 'crt.o'
    command(['nasm', '-f', 'elf32', ROOT / 'tests/i8086/crt0.asm', '-o', crt])
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(check_seed, seed, output, args.dcc.resolve(), crt, native, args.minimize) for seed in seeds]
        reports = [future.result() for future in futures]
    report = dict(configurations=CONFIGS, seeds=seeds, failures=[r for r in reports if r['failures'] or not r['reference_valid']],
                  revision=command(['git', '-C', ROOT, 'rev-parse', 'HEAD']).stdout.decode().strip(),
                  generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(reports) - len(report["failures"])}/{len(reports)} seeds, {len(reports) * len(CONFIGS)} executions; evidence {output}')
    if temporary:
        if report['failures']:
            temporary.cleanup = lambda: None
            temporary._finalizer.detach()
        else:
            temporary.cleanup()
    return bool(report['failures'])


if __name__ == '__main__':
    sys.exit(main())
