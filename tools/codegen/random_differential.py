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
    loop_trips = rng.randrange(6)
    loop_scale = rng.choice((3, 5, 7, 9))
    wide_index_type = 'i64' if seed & 1 else 'u64'
    functions.append(f'''@noinline @runtime u32 wide_index([]u16 data, {wide_index_type} count) {{
    u32 sum = 0;
    for {wide_index_type} i = 0; i < count; i += 1 {{
        sum ^= data[((i * 3) as usize)] as u32;
        sum += i as u32;
    }}
    return sum;
}}
@noinline @runtime u32 invariant_loops(u32 x, u16 trips, u32* settings) {{
    u16[32] data;
    for usize i = 0; i < 32; i += 1 {{ data[i] = (i as u16) + ((x & 15) as u16); }}
    u32 result = 0;
    for usize i = 0; i < (trips as usize); i += 1 {{
        u32 value = (x * {loop_scale} + 17) ^ (x >> 2);
        result += value + *settings;
        if (i & 1) == 0 {{ result ^= (x * {loop_scale} + 17) ^ (x >> 2); }}
        result += data[i * 3] as u32;
        result ^= data[(i << 1) + 1] as u32;
        for usize j = 0; j < 2; j += 1 {{ result += value + (data[i * 3 + j] as u32); }}
    }}
    return result ^ wide_index(data[0..32], trips as {wide_index_type});
}}
@noinline @runtime u32 invariant_alias(u32* read, u32* write, u16 trips) {{
    u32 result = 0;
    for u16 i = 0; i < trips; i += 1 {{ result += *read; *write += 3; result ^= *read; }}
    return result;
}}
@noinline @runtime u32 backedge_alias(u32* pointer, u16 trips) {{
    u32 result = *pointer;
    for u16 i = 0; i < trips; i += 1 {{ result += *pointer; *pointer += 3; }}
    return result;
}}
@noinline @runtime u32 zero_trip(u32* pointer, u32 divisor, u16 trips) {{
    u32 result = 0;
    for u16 i = 0; i < trips; i += 1 {{ result += *pointer + (19 / divisor); }}
    return result;
}}
@noinline @runtime u32 loop_shapes(u32 x) {{
    u32 first = x + 13;
    u32 second = x + 29;
    u16 trips = ((x & 3) as u16) + {loop_trips};
    if trips > 5 {{ trips = 5; }}
    u32 result = invariant_loops(x, trips, &first);
    result ^= invariant_alias(&first, &second, trips);
    result ^= invariant_alias(&first, &first, trips);
    result ^= backedge_alias(&first, trips);
    result ^= zero_trip(null, 0, 0);
    result ^= zero_trip(&second, 3, trips);
    return result ^ first ^ second;
}}''')
    blocks.append('mix(&checksum, loop_shapes(input));')
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


FLOAT_MARKER = 0xf1000000
FLOAT_CONFIGS = ('llvm-O0', 'llvm-O2', 'custom-O0', 'custom-O2')
INTEGER_TYPES = ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64')
C_TYPES = dict(zip(INTEGER_TYPES, ('int8_t', 'uint8_t', 'int16_t', 'uint16_t',
                                'int32_t', 'uint32_t', 'int64_t', 'uint64_t')),
               f32='float', f64='double')


def float_abi(seed):
    rng = random.Random(seed ^ 0xF10A7)
    declarations, functions, c_functions, blocks = [], [], [], []
    for abi, cc, attribute in (('sysv', 'SystemV', 'sysv_abi'), ('win64', 'Win64', 'ms_abi')):
        types = ['f32', 'i8', 'f64', 'u64', 'f32', 'i16', 'f64', 'u32',
                 'f32', 'i64', 'f64', 'u8', 'f32', 'u16', 'f64', 'i32', 'f32', 'f64']
        rng.shuffle(types)
        types += [rng.choice(tuple(C_TYPES)) for _ in range(rng.randrange(5))]
        values = [rng.randint(1, 15) for _ in types]
        weights = [rng.randint(1, 7) for _ in types]
        dc_params = ', '.join(f'{t} p{i}' for i, t in enumerate(types))
        c_params = ', '.join(f'{C_TYPES[t]} p{i}' for i, t in enumerate(types))
        dc_sum = ' + '.join(f'(p{i} as f64) * {w}.0' for i, w in enumerate(weights))
        c_sum = ' + '.join(f'(double)p{i} * {w}.0' for i, w in enumerate(weights))
        dc_args = ', '.join(f'({v} as {t})' for v, t in zip(values, types))
        c_args = ', '.join(f'({C_TYPES[t]}){v}' for v, t in zip(values, types))
        attrs = f'@nomangle @calling_conv("{cc}")'
        declarations.append(f'{attrs} extern f64 c_mix_{abi}({dc_params});\n@nomangle extern f64 c_call_{abi}();')
        functions.append(f'{attrs} @noinline @runtime public f64 dc_mix_{abi}({dc_params}) {{ return {dc_sum}; }}')
        c_functions.append(f'''extern double __attribute__(({attribute})) dc_mix_{abi}({c_params});
__attribute__(({attribute}, noinline)) double c_mix_{abi}({c_params}) {{ return {c_sum}; }}
double c_call_{abi}(void) {{ return dc_mix_{abi}({c_args}); }}''')
        expected = sum(v * w for v, w in zip(values, weights))
        blocks.append(f'''observe64(c_mix_{abi}({dc_args}));
    observe64(c_call_{abi}());
    emit((c_mix_{abi}({dc_args}) == {expected}.0) as u64);
    emit((c_call_{abi}() == {expected}.0) as u64);''')
        clobbers = range(16) if abi == 'sysv' else range(6)
        assembly = ''.join(f'pxor %%xmm{i}, %%xmm{i};' for i in clobbers)
        registers = ', '.join(f'"xmm{i}"' for i in clobbers)
        c_functions.append(f'''__attribute__(({attribute}, noinline)) uint32_t c_clobber_{abi}(uint32_t x) {{
    __asm__ volatile("{assembly}" : : : {registers});
    return x + 3;
}}''')
        declarations.append(f'{attrs} extern u32 c_clobber_{abi}(u32 x);')
        for typ in ('f32', 'f64'):
            declarations.append(f'{attrs} extern void c_warm_{typ}_{abi}({typ} x);')
            c_functions.append(f'''__attribute__(({attribute}, noinline)) void c_warm_{typ}_{abi}({C_TYPES[typ]} x) {{
    __asm__ volatile("" : : "x"(x));
}}''')
    return '\n'.join(declarations), '\n'.join(functions), '\n'.join(c_functions), blocks


def generate_float(seed, keep=None, labels=None):
    rng = random.Random(seed ^ 0xF32F64)
    declarations, abi_functions, _, blocks = float_abi(seed)
    functions = [abi_functions]
    header = f'''module main;
union Bits32 {{ f32 value; u32 bits; }}
union Bits64 {{ f64 value; u64 bits; }}
public struct Pair32 {{ f32 first; f32 second; }}
public struct Pair64 {{ f64 first; f64 second; }}
public struct Mixed {{ f64 wide; i32 integer; f32 narrow; }}
volatile u32 initial = {(seed & 15) + 1};
volatile u32 trip_count = {(seed % 5) + 2};
const char[17] digits = "0123456789abcdef";
@nomangle extern void dcc_putc(u8 value);
@noinline @runtime void emit(u64 value) {{
    for u32 i = 0; i < 16; i += 1 {{ dcc_putc(digits[((value >> (((15 - i) * 4) as u64)) & 15) as usize] as u8); }}
    dcc_putc(10);
}}
@noinline @runtime void observe32(f32 value) {{
    Bits32 bits = {{value = value}};
    if value == 0.0 {{ emit(0); }} else {{ emit(bits.bits as u64); }}
}}
@noinline @runtime void observe64(f64 value) {{
    Bits64 bits = {{value = value}};
    if value == 0.0 {{ emit(0); }} else {{ emit(bits.bits); }}
}}
{declarations}
'''
    for typ, observe, pair in (('f32', 'observe32', 'Pair32'), ('f64', 'observe64', 'Pair64')):
        quarter = rng.randint(1, 31) / 4
        factor = rng.choice((0.5, 2.0, 4.0))
        divisor = rng.choice((2.0, 4.0, 8.0))
        for op, name in (('+', 'add'), ('-', 'subtract'), ('*', 'multiply'), ('/', 'divide')):
            functions.append(f'@noinline @runtime {typ} {name}_{typ}({typ} a, {typ} b) {{ return a {op} b; }}')
            operand = divisor if op == '/' else factor if op == '*' else quarter
            blocks.append(f'{observe}({name}_{typ}((input as {typ}) + {quarter}, {operand}));')
        relations = ('<', '<=', '>', '>=', '==', '!=')
        comparisons = ' | '.join(f'(((a {op} b) as u64) << {i})' for i, op in enumerate(relations))
        functions.append(f'@noinline @runtime u64 compare_{typ}({typ} a, {typ} b) {{ return {comparisons}; }}')
        blocks.append(f'''emit(compare_{typ}(input as {typ}, (input as {typ}) + 0.25));
    emit(compare_{typ}(input as {typ}, input as {typ}));
    emit(compare_{typ}((input as {typ}) + 0.25, input as {typ}));''')
        functions.append(f'''@noinline @runtime {typ} phis_{typ}(u32 x, u32 trips, bool choose) {{
    {typ} a = {quarter};
    {typ} b = (x as {typ}) * 0.5;
    if choose {{ a = {quarter + 1.0}; }} else {{ a = b + 0.25; }}
    for u32 i = 0; i < trips; i += 1 {{
        if (i & 1) == 0 {{ a += 0.5; }} else {{ a = b + 0.25; }}
        b += 0.25;
    }}
    return a + b;
}}
@noinline @runtime {typ} constant_loop_{typ}(u32 trips) {{
    {typ} value = {quarter};
    for u32 i = 0; i < trips; i += 1 {{ value += 0.5; }}
    return value;
}}
@noinline @runtime {typ} constant_branch_{typ}(bool choose) {{
    return if choose {{ {quarter} as {typ} }} else {{ {quarter + 0.5} as {typ} }};
}}
@noinline @runtime {typ} identity_{typ}({typ} value) {{ return value; }}''')
        blocks.append(f'''{observe}(phis_{typ}(input, trip_count, true));
    {observe}(phis_{typ}(input, trip_count, false));
''')
        blocks.append(f'''{observe}(constant_loop_{typ}(trip_count));
    {observe}(constant_loop_{typ}(0));''')
        blocks.append(f'''{observe}(constant_branch_{typ}((input & 1) != 0));
    {observe}(constant_branch_{typ}((input & 1) == 0));''')
        for abi, cc in (('sysv', 'SystemV'), ('win64', 'Win64')):
            attrs = f'@nomangle @calling_conv("{cc}") @noinline @runtime'
            locals_ = '\n    '.join(f'{typ} v{i} = (x as {typ}) * 0.5 + {i + quarter};' for i in range(20))
            total = ' + '.join(f'v{i}' for i in range(20))
            functions.append(f'''{attrs} public {typ} live_{typ}_{abi}(u32 x) {{
    {locals_}
    c_warm_{typ}_{abi}(v19);
    u32 tick = c_clobber_{abi}(x);
    {typ} second = identity_{typ}(v19);
    return second + ({total}) + (tick as {typ});
}}
{attrs} public {pair} pair_{typ}_{abi}({pair} value, {typ} delta) {{
    value.first += delta;
    value.second = value.second * 2.0 - delta;
    return value;
}}
{attrs} public {typ}[4] array_{typ}_{abi}({typ}[4] value, {typ} delta) {{
    for usize i = 0; i < 4; i += 1 {{ value[i] = value[i] * 2.0 + delta; }}
    return value;
}}''')
            blocks.append(f'''{observe}(live_{typ}_{abi}(input));
    {pair} pair_{typ}_{abi}_value = {{first = (input as {typ}) + 0.25, second = {quarter}}};
    {pair} pair_{typ}_{abi}_result = pair_{typ}_{abi}(pair_{typ}_{abi}_value, 0.5);
    {observe}(pair_{typ}_{abi}_result.first);
    {observe}(pair_{typ}_{abi}_result.second);
    {typ}[4] array_{typ}_{abi}_value = {{0.25, {quarter}, 1.5, input as {typ}}};
    {typ}[4] array_{typ}_{abi}_result = array_{typ}_{abi}(array_{typ}_{abi}_value, 0.5);
    for usize i = 0; i < 4; i += 1 {{ {observe}(array_{typ}_{abi}_result[i]); }}''')
        for integer in INTEGER_TYPES:
            width = int(integer[1:])
            signed = integer.startswith('i')
            exponent = width - (2 if signed else 1)
            value = (1 << exponent) + (rng.randrange(4) << max(0, exponent - 3))
            if signed:
                value = -value
            fraction = -3.75 if signed else 3.75
            functions.append(f'''@noinline @runtime void convert_{typ}_{integer}({integer} value, {typ} fraction) {{
    {typ} converted = value as {typ};
    {integer} restored = converted as {integer};
    {integer} truncated = fraction as {integer};
    {observe}(converted);
    emit(restored as u64);
    emit(truncated as u64);
    emit((restored == value) as u64);
}}''')
            blocks.append(f'convert_{typ}_{integer}(({value} as {integer}) + ((input & 0) as {integer}), {fraction});')
    for abi, cc in (('sysv', 'SystemV'), ('win64', 'Win64')):
        functions.append(f'''@nomangle @calling_conv("{cc}") @noinline @runtime public Mixed mixed_{abi}(Mixed value, f32 x, i32 y, f64 z) {{
    value.wide = value.wide + z;
    value.narrow = value.narrow * 2.0 + x;
    value.integer += y;
    return value;
}}''')
        blocks.append(f'''Mixed mixed_{abi}_value = {{wide = (input as f64) + 0.25, integer = input as i32, narrow = 1.5}};
    Mixed mixed_{abi}_result = mixed_{abi}(mixed_{abi}_value, 0.5, 3, 0.25);
    observe64(mixed_{abi}_result.wide);
    observe32(mixed_{abi}_result.narrow);
    emit(mixed_{abi}_result.integer as u64);''')
    chosen = range(len(blocks)) if keep is None else keep
    if labels is not None:
        labels.extend(block.strip().splitlines()[0] for block in blocks)
    body = '\n    '.join(f'emit({FLOAT_MARKER + i});\n    ' + blocks[i] for i in chosen)
    return header + '\n'.join(functions) + f'''\n@nomangle public i32 dcc_main() {{
    u32 input = initial;
    {body}
    return 0;
}}\n''', len(blocks)


def float_native(seed):
    _, _, functions, _ = float_abi(seed)
    return '#include <stdint.h>\n#include <stdio.h>\n' + functions + '''
int dcc_main(void);
void dcc_putc(unsigned char x) { putchar(x); }
int main(void) { return dcc_main(); }
'''


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
        command(['clang', '-ffp-contract=off', native, obj, '-o', binary])
        result = subprocess.run([binary], capture_output=True, timeout=10)
        return dict(exit=result.returncode, output=result.stdout.hex(), stderr=result.stderr.decode(errors='replace'))
    binary = directory / (config + '.bin')
    command([dcc, '-target', 'i8086-binary', '-mcmodel', backend, '-fbackend', 'custom', '-' + opt,
             '-c', directory / 'main.dc', '-o', obj])
    command([dcc, '-target', 'i8086-binary', '-mcmodel', backend, '-fbase', '1000:0000', crt, obj, '-o', binary])
    result = run_guest(binary.read_bytes(), mode='real' if backend == 'small' else 'unreal', timeout=10,
                       workdir=directory)
    return dict(exit=result.exit_code, output=result.serial.hex(), stderr=result.error or '', timeout=result.timed_out)


def float_observations(record, block_count):
    observations = {}
    block = None
    for line in bytes.fromhex(record.get('output', '')).splitlines():
        if len(line) != 16 or any(c not in b'0123456789abcdef' for c in line):
            continue
        value = int(line, 16)
        if FLOAT_MARKER <= value < FLOAT_MARKER + block_count:
            block = value - FLOAT_MARKER
            observations[block] = []
        elif block is not None:
            observations[block].append(value)
    return observations


def float_invariants(record, labels):
    failures = []
    for block, values in float_observations(record, len(labels)).items():
        label = labels[block]
        if label.startswith('convert_'):
            signed = label.split('_')[2].startswith('i')
            expected_truncation = (-3 if signed else 3) & ((1 << 64) - 1)
            if len(values) != 4 or values[-1] != 1 or values[2] != expected_truncation:
                failures.append(block)
        elif 'c_mix_' in label:
            if len(values) != 4 or values[2:] != [1, 1]:
                failures.append(block)
    return failures


def check_program(seed, directory, dcc, crt, native, minimize, family):
    directory.mkdir()
    generator = generate_float if family == 'float' else generate
    configs = FLOAT_CONFIGS if family == 'float' else CONFIGS
    if family == 'float':
        native = directory / 'native.c'
        native.write_text(float_native(seed))
    labels = []
    source, count = generator(seed, labels=labels) if family == 'float' else generator(seed)
    (directory / 'main.dc').write_text(source)
    records = {}
    for config in configs:
        try:
            records[config] = execute(config, directory, dcc, crt, native)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            records[config] = dict(error=str(error))
    if family == 'float':
        for record in records.values():
            record['invariant_failures'] = float_invariants(record, labels)
    reference = records[configs[0]]
    data = bytes.fromhex(reference.get('output', ''))
    valid = (reference.get('exit') == 0 and not reference.get('stderr') and
             (len(data) == 9 if family == 'integer' else bool(data) and len(data) % 17 == 0) and
             data.endswith(b'\n') and all(len(line) == (8 if family == 'integer' else 16) and
             all(c in b'0123456789abcdef' for c in line) for line in data.splitlines()))
    failures = [c for c in configs if not valid or records[c].get('invariant_failures') or records[c].get('exit') != 0 or
                records[c].get('output') != reference.get('output') or records[c].get('stderr') or records[c].get('timeout')]
    minimized = None
    if failures and valid and minimize:
        target = next((c for c in failures if c != configs[0] and records[c].get('output') != reference.get('output')), failures[0])
        keep = list(range(count))
        trial = directory / 'minimize'
        trial.mkdir()
        for block in range(count):
            candidate = [i for i in keep if i != block]
            text, _ = generator(seed, candidate)
            (trial / 'main.dc').write_text(text)
            try:
                a = execute(configs[0], trial, dcc, crt, native)
                b = execute(target, trial, dcc, crt, native)
                if a.get('exit') == 0 and (b.get('exit') != 0 or a.get('output') != b.get('output') or
                        (family == 'float' and float_invariants(b, labels))):
                    keep = candidate
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
        text, _ = generator(seed, keep)
        (directory / 'minimized.dc').write_text(text)
        minimized = dict(configuration=target, retained_blocks=keep)
    differing_blocks = {}
    if family == 'float':
        expected = float_observations(reference, count)
        differing_blocks = {c: [i for i in expected if float_observations(r, count).get(i) != expected[i]]
                            for c, r in records.items()}
    report = dict(seed=seed, differing_blocks=differing_blocks, block_labels=labels, source_sha256=hashlib.sha256(source.encode()).hexdigest(), records=records, failures=failures, reference_valid=valid, reference_invariants_valid=not reference.get('invariant_failures'), minimized=minimized)
    (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{family} seed {seed}: ' + ('PASS' if valid and not failures else 'FAIL ' + ', '.join(failures)), flush=True)
    return report


def check_seed(seed, output, dcc, crt, native, minimize, families):
    directory = output / f'seed-{seed}'
    directory.mkdir()
    programs = {family: check_program(seed, directory / family, dcc, crt, native, minimize, family) for family in families}
    return dict(seed=seed, programs=programs,
                failed=any(p['failures'] or not p['reference_valid'] for p in programs.values()))


def main():
    parser = argparse.ArgumentParser(description='Seeded integer and exact floating-point execution differential, including SysV/Win64 C ABI boundaries.')
    parser.add_argument('--dcc', type=Path, default=ROOT / 'build/bin/dcc')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--seeds', nargs='+', type=lambda s: int(s, 0))
    parser.add_argument('--count', type=int, help='consecutive seeds beginning at --start; default uses the fixed suite')
    parser.add_argument('--start', type=int, default=0)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--families', nargs='+', choices=('integer', 'float'), default=('integer', 'float'))
    parser.add_argument('--minimize', action='store_true', help='greedily remove observation blocks from failing programs')
    args = parser.parse_args()
    seeds = args.seeds if args.seeds is not None else list(range(args.start, args.start + args.count)) if args.count else FIXED_SEEDS
    if len(set(seeds)) != len(seeds) or len(set(args.families)) != len(args.families) or args.jobs < 1:
        parser.error('seeds and families must be unique and jobs positive')
    if args.count is not None and args.count < 1:
        parser.error('count must be positive')
    generator_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    compiler_hash = hashlib.sha256(args.dcc.read_bytes()).hexdigest()
    temporary = tempfile.TemporaryDirectory(prefix='dcc-random-differential-') if args.output is None else None
    output = (args.output or Path(temporary.name)).resolve()
    output.mkdir(parents=True, exist_ok=True)
    native = output / 'native.c'
    native.write_text('#include <stdio.h>\nint dcc_main(void);\nvoid dcc_putc(unsigned char x) { putchar(x); }\nint main(void) { return dcc_main(); }\n')
    crt = output / 'crt.o'
    if 'integer' in args.families:
        command(['nasm', '-f', 'elf32', ROOT / 'tests/i8086/crt0.asm', '-o', crt])
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(check_seed, seed, output, args.dcc.resolve(), crt, native, args.minimize, args.families) for seed in seeds]
        reports = [future.result() for future in futures]
    report = dict(configurations={f: FLOAT_CONFIGS if f == 'float' else CONFIGS for f in args.families}, seeds=seeds, failures=[r for r in reports if r['failed']],
                  revision=command(['git', '-C', ROOT, 'rev-parse', 'HEAD']).stdout.decode().strip(),
                  compiler_path=str(args.dcc.resolve()), compiler_sha256=compiler_hash,
                  generator_sha256=generator_hash)
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(reports) - len(report["failures"])}/{len(reports)} seeds, {len(reports) * sum(len(FLOAT_CONFIGS if f == 'float' else CONFIGS) for f in args.families)} executions; evidence {output}')
    if temporary:
        if report['failures']:
            temporary.cleanup = lambda: None
            temporary._finalizer.detach()
        else:
            temporary.cleanup()
    return bool(report['failures'])


if __name__ == '__main__':
    sys.exit(main())
