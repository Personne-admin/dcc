#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]
ANSI = re.compile(r'\x1b\[[0-9;]*m')
MEMORY = ('memcpy', 'memmove', 'memset', 'memcmp')
FIXTURE = ROOT / 'tests/stdlib/static-allocators.dc'
MAIN_LINE = 'public i32 main() { return dcc_main(); }'


def audit_object(obj, target):
    allowed = set(MEMORY)
    if target.endswith('coff'):
        allowed.update('_' + name for name in MEMORY)
        allowed.add('__chkstk')
    for symbol in run(['nm', '-u', obj]).splitlines():
        assert symbol.split()[-1] in allowed, (obj, symbol)


def native(dcc, scratch, windows):
    target = 'x86_64-coff' if windows else 'x86_64-elf'
    variant = 'windows' if windows else 'linux'
    mingw = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'x86_64-w64-mingw32/lib'
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            for whole in (False, True):
                obj = scratch / 'test.o'
                exe = scratch / ('test.exe' if windows else 'test')
                flags = [dcc, '-flibdcext', variant, '-target', target, '-fbackend', backend, '-' + opt, '-I', ROOT / 'libdcext']
                if whole:
                    flags += ['-fwhole-program']
                if windows:
                    run([*flags, '-c', FIXTURE, '-o', obj])
                    run([dcc, '-target', target, '-flibdcext', variant, '-fbackend', backend,
                         *(['-fwhole-program'] if whole else []), '-L', mingw, obj,
                         '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe])
                    run([os.environ.get('WINE', 'wine'), exe])
                else:
                    if whole:
                        run([*flags, FIXTURE, '-o', exe])
                    else:
                        run([*flags, '-c', FIXTURE, '-o', obj])
                        run([dcc, '-flibdcext', variant, '-fbackend', backend, obj, '-o', exe])
                    run([exe])
                print('  PASS static allocators', variant, backend, opt, 'whole' if whole else 'separate', flush=True)


def portable(dcc, scratch):
    source = scratch / 'portable.dc'
    source.write_text(FIXTURE.read_text().replace(MAIN_LINE, ''))
    compiles = 0
    for target in ('x86-elf', 'x86-coff', 'x86_64-elf', 'x86_64-coff'):
        backends = ('llvm', 'custom') if target.startswith('x86_64') else ('llvm',)
        for backend in backends:
            for opt in ('O0', 'O2'):
                obj = scratch / 'portable.o'
                run([dcc, '-target', target, '-fbackend', backend, '-' + opt, '-I', ROOT / 'libdcext',
                     '-fwhole-program', '-e', 'dcc_main', '-fno-simd', '-fno-x87', '-c', source, '-o', obj])
                audit_object(obj, target)
                compiles += 1
    startup = ROOT / 'tests/i8086/crt0.dc'
    assert startup.exists(), startup
    entry = scratch / 'entry.dc'
    entry.write_text(source.read_text() + '\n' + startup.read_text())
    for model in ('small', 'unreal', 'unreal32'):
        for opt in ('O0', 'O2'):
            binary = scratch / 'portable.bin'
            run([dcc, '-target', 'i8086-binary', '-fbackend', 'custom', '-mcmodel=' + model, '-' + opt,
                 '-I', ROOT / 'libdcext', '-e', '_start', entry, '-o', binary])
            run(['python3', ROOT / 'tests/i8086/run.py', binary, '--mode', 'unreal' if model != 'small' else 'real'])
            compiles += 1
            print('  PASS static allocators i8086', model, opt, flush=True)
    return compiles


NEGATIVE = '''module main;

import std::alloc;
import std::result;

using std::result::Result;
using std::alloc::{ AllocError, AllocatorPolicy };

struct MissingResize {}

Result([] u8, AllocError) allocate(MissingResize* self, usize len, usize align) {
    return AllocError::OutOfMemory;
}

void free(MissingResize* self, [] u8 block, usize align) {}

void require_allocator(A)(A* self) if AllocatorPolicy(A) {}

public i32 main() {
    MissingResize policy = {};
    require_allocator(&policy);
    return 0;
}
'''


ARENA_NEGATIVE = '''module main;

public import std::alloc;
public import std::alloc::arena;
public import std::result;

using std::result::{ Result, Optional };
using std::alloc::AllocError;
using std::alloc::arena::new_arena;

public struct BadParent {
    usize x;
}

public i32 main() {
    BadParent bad = {};
    new_arena(&bad);
    return 0;
}
'''


ZERO_POLICY = '''module zero;
public import std::result;
public import std::alloc;
using std::result::Result;
using std::alloc::AllocError;

public struct Z {
}

public u8[65536] pool;
public usize used;

public Result([] u8, AllocError) allocate(Z* self, usize len, usize align) {
    if used + len > 65536 {
        return AllocError::OutOfMemory;
    }
    [] u8 out = pool[used..used + len];
    used = used + len;
    return Result::Ok(out);
}

public bool resize_in_place(Z* self, [] u8 block, usize new_bytes, usize align) {
    return false;
}

public void free(Z* self, [] u8 block, usize align) {
}
'''

GENERIC_LIBRARY = '''module lib;
public import std::array;

public i32 take(T)(std::array::Array(T)* a) {
    return a.len() as i32;
}

public std::array::Array(T) make(T)() {
    return std::array::new_array();
}
'''

GENERIC_PROGRAM = '''module main;
import std::array;
import lib;

public i32 main() {
    std::array::Array(i32) a = lib::make();
    if !a.push(5).is_ok() {
        return 1;
    }
    if lib::take(&a) != 1 {
        return 2;
    }
    a.deinit();
    return 0;
}
'''


def generic_signature_defaults(dcc, scratch):
    (scratch / 'zero.dc').write_text(ZERO_POLICY)
    (scratch / 'lib.dc').write_text(GENERIC_LIBRARY)
    (scratch / 'prog.dc').write_text(GENERIC_PROGRAM)

    include = ['-I', scratch, '-I', ROOT / 'libdcext']
    library_obj = scratch / 'lib.o'
    policy_obj = scratch / 'zero.o'
    run([dcc, *include, '-flibdcext', 'freestanding', '-target', 'x86_64-elf', '-fbackend', 'llvm', '-O0',
         '-c', scratch / 'lib.dc', '-o', library_obj])
    run([dcc, *include, '-flibdcext', 'freestanding', '-target', 'x86_64-elf', '-fbackend', 'llvm', '-O0',
         '-c', scratch / 'zero.dc', '-o', policy_obj])
    archive = scratch / 'lib.a'
    run(['ar', 'rcs', archive, library_obj, policy_obj])

    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            exe = scratch / 'generic-exe'
            select = '-fdefault-allocator=zero::Z'
            run([dcc, *include, '-flibdcext', 'linux', '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt,
                 '-fwhole-program', select, scratch / 'prog.dc', '-o', exe])
            run([exe])

            program_obj = scratch / 'prog.o'
            run([dcc, *include, '-flibdcext', 'linux', '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt,
                 select, '-c', scratch / 'prog.dc', '-o', program_obj])
            run([dcc, '-flibdcext', 'linux', '-target', 'x86_64-elf', '-fbackend', backend,
                 program_obj, archive, '-o', exe])
            run([exe])

    result = subprocess.run(['timeout', '60', str(dcc), *map(str, include), '-flibdcext', 'linux', '-target', 'x86_64-elf',
                             '-fbackend', 'llvm', '-O0', '-fwhole-program', scratch / 'prog.dc', '-o', scratch / 'no-selection'],
                            capture_output=True, text=True, timeout=65)
    text = ANSI.sub('', result.stderr)
    assert result.returncode == 1, text
    assert 'no default allocator selected' in text, text
    print('  PASS static allocators generic signature default selection', flush=True)


def negative(dcc, scratch):
    source = scratch / 'negative.dc'
    source.write_text(NEGATIVE)
    arena_source = scratch / 'negative-arena.dc'
    arena_source.write_text(ARENA_NEGATIVE)
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            result = subprocess.run(['timeout', '60', str(dcc), '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt,
                                     '-I', ROOT / 'libdcext', '-c', source, '-o', scratch / 'negative.o'],
                                    capture_output=True, text=True, timeout=65)
            text = ANSI.sub('', result.stderr)
            assert result.returncode == 1, (backend, opt, text)
            errors = [line for line in text.splitlines() if line.startswith('error:')]
            assert errors == ['error: template constraint not satisfied for `require_allocator`'], (backend, opt, errors)

            result = subprocess.run(['timeout', '60', str(dcc), '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt,
                                     '-I', ROOT / 'libdcext', '-c', arena_source, '-o', scratch / 'negative-arena.o'],
                                    capture_output=True, text=True, timeout=65)
            text = ANSI.sub('', result.stderr)
            assert result.returncode == 1, (backend, opt, text)
            errors = [line for line in text.splitlines() if line.startswith('error:')]
            assert errors == ['error: template constraint not satisfied for `new_arena`'], (backend, opt, errors)
    print('  PASS static allocators allocator-contract diagnostics', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    with tempfile.TemporaryDirectory(prefix='dcc-static-allocators-') as temporary:
        scratch = Path(temporary)
        native(dcc, scratch, args.windows)
        if not args.windows:
            compiles = portable(dcc, scratch)
            negative(dcc, scratch)
            generic_signature_defaults(dcc, scratch)
            print('  RESULT static allocator executions, ' + str(compiles) + ' narrow compiles and contract diagnostics passed', flush=True)
        else:
            print('  RESULT static allocator executions passed', flush=True)


if __name__ == '__main__':
    main()
