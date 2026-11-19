#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HOSTED = ROOT / 'tests/backend/seh/hosted.dc'
FREESTANDING = ROOT / 'tests/backend/seh/freestanding.dc'


def run(argv, expect=0, **kwargs):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=120, **kwargs)
    assert result.returncode == expect, (argv, result.returncode, result.stdout, result.stderr)
    return result


def relocations(obj):
    return run(['llvm-objdump', '-r', obj]).stdout


def drectve(obj):
    dump = run(['llvm-objdump', '-s', '-j', '.drectve', obj]).stdout
    text = ''
    for line in dump.splitlines():
        parts = line.split()
        if len(parts) >= 2 and re.fullmatch(r'[0-9a-f]{4}', parts[0]):
            for word in parts[1:5]:
                if re.fullmatch(r'[0-9a-f]+', word):
                    text += bytes.fromhex(word).decode('latin-1')
    return text


def check_hosted_objects(dcc, scratch):
    checks = 0
    for flags in (['-O0'], ['-O2'], ['-Os'], ['-O2', '-g'], ['-O2', '-g', '-gdwarf'], ['-O2', '-ffunction-sections', '-fdata-sections'],
                  ['-O2', '-fwhole-program']):
        obj = scratch / ('hosted' + ''.join(flags).replace('-', '_') + '.o')
        result = subprocess.run([str(dcc), '-target', 'x86_64-coff', '-flibdcext', 'windows', '-fseh', *flags, '-c', str(HOSTED), '-o', str(obj)],
                                capture_output=True, text=True, timeout=120)
        if '-gdwarf' in flags and result.returncode != 0 and 'unknown option' in result.stderr:
            continue
        assert result.returncode == 0, (flags, result.stdout, result.stderr)
        relocs = relocations(obj)
        assert 'IMAGE_REL_AMD64_ADDR32NB __C_specific_handler' in relocs, (flags, relocs)
        assert re.search(r'IMAGE_REL_AMD64_ADDR32NB \S*\$seh_filter\d+', relocs), (flags, relocs)
        headers = run(['llvm-objdump', '-h', obj]).stdout
        assert '.pdata' in headers and '.xdata' in headers, headers
        assert '__dcext_C_specific_handler' not in run(['llvm-nm', obj]).stdout, flags
        checks += 1
    return checks


def check_freestanding(dcc, scratch):
    checks = 0
    stub = scratch / 'stub.c'
    stub.write_text('void RtlUnwindEx(void) {}\nvoid _start(void) {}\n')
    stub_obj = scratch / 'stub.o'
    run(['clang', '-target', 'x86_64-pc-windows-msvc', '-c', stub, '-o', stub_obj])
    for runtime in ('libdcext', 'external'):
        for whole in (True, False):
            obj = scratch / ('freestanding-' + runtime + ('-whole' if whole else '') + '.o')
            run([dcc, '-target', 'x86_64-coff', '-flibdcext', 'freestanding', '-fseh', '-fseh-runtime=' + runtime,
                 *(['-fwhole-program', '-e', 'user_entry'] if whole else []), '-O2', '-c', FREESTANDING, '-o', obj])
            directives = drectve(obj)
            symbols = run(['llvm-nm', obj]).stdout
            if runtime == 'libdcext':
                assert '/include:__dcext_C_specific_handler' in directives, directives
                assert (' T __dcext_C_specific_handler' in symbols) == whole, symbols
                assert re.search(r'\b[TW] __C_specific_handler\b', symbols), symbols
            else:
                assert '__dcext_C_specific_handler' not in directives + symbols, (directives, symbols)
                assert re.search(r'\bU __C_specific_handler\b', symbols), symbols
            checks += 1
            if not whole:
                continue
            image = scratch / ('freestanding-' + runtime + '.exe')
            inputs = [obj, stub_obj]
            if runtime == 'external':
                handler = scratch / 'handler.c'
                handler.write_text('int __C_specific_handler(void) { return 1; }\n')
                handler_obj = scratch / 'handler.o'
                run(['clang', '-target', 'x86_64-pc-windows-msvc', '-c', handler, '-o', handler_obj])
                inputs.append(handler_obj)
            run(['ld.lld', '-m', 'i386pep', '--subsystem', 'console', '--entry', '_start', *inputs, '-o', image])
            unwind = run(['llvm-readobj', '--unwind', image]).stdout
            assert re.search(r'Handler: __C_specific_handler \(', unwind), unwind
            image_symbols = run(['llvm-objdump', '-t', image]).stdout
            assert ('__dcext_C_specific_handler' in image_symbols) == (runtime == 'libdcext'), image_symbols
            checks += 1
    return checks


def check_diagnostics(dcc, scratch):
    checks = 0
    custom = run([dcc, '-target', 'x86_64-coff', '-fseh', '-fbackend', 'custom', '-c', FREESTANDING, '-o', scratch / 'custom.o'], expect=1)
    assert '-fseh requires the LLVM backend' in custom.stderr, custom.stderr
    checks += 1
    nolib = run([dcc, '-target', 'x86_64-coff', '-fseh', '-fseh-runtime=libdcext', '-c', FREESTANDING, '-o', scratch / 'nolib.o'], expect=1)
    assert '-fseh-runtime=libdcext requires -flibdcext' in nolib.stderr, nolib.stderr
    checks += 1
    plain = scratch / 'plain.o'
    run([dcc, '-target', 'x86_64-coff', '-O2', '-c', ROOT / 'tests/backend/seh/defer_only.dc', '-o', plain])
    assert '__C_specific_handler' not in relocations(plain)
    seh = scratch / 'seh.o'
    run([dcc, '-target', 'x86_64-coff', '-fseh', '-O2', '-c', ROOT / 'tests/backend/seh/defer_only.dc', '-o', seh])
    assert 'IMAGE_REL_AMD64_ADDR32NB __C_specific_handler' in relocations(seh)
    checks += 1
    return checks


def execute_hosted(dcc, scratch):
    sysroot = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'x86_64-w64-mingw32/lib'
    wine = os.environ.get('WINE', 'wine')
    checks = 0
    for runtime in ('external', 'libdcext'):
        for level in ('-O0', '-O2', '-Os'):
            for whole in ('-fwhole-program', '-fno-whole-program'):
                exe = scratch / ('hosted-' + runtime + level + whole + '.exe')
                run([dcc, '-target', 'x86_64-coff', '-flibdcext', 'windows', '-fseh', '-fseh-runtime=' + runtime, level, whole, HOSTED,
                     '-L', sysroot, '-o', exe])
                unwind = run(['llvm-readobj', '--unwind', exe]).stdout
                handlers = set(int(v, 16) for v in re.findall(r'Handler: \S+ \((0x[0-9a-fA-F]+)\)', unwind))
                symbols = dict((name, int(value, 16)) for value, name in re.findall(r'(0x[0-9a-f]+) (\S+)$', run(['llvm-objdump', '-t', exe]).stdout, re.M))
                assert len(handlers) == 1, (runtime, level, whole, handlers)
                imported = '__imp___C_specific_handler' in symbols
                assert imported == (runtime == 'external'), (runtime, level, whole, imported)
                assert ('__dcext_C_specific_handler' in symbols) == (runtime == 'libdcext'), (runtime, level, whole)
                result = subprocess.run(['timeout', '60', wine, str(exe)], capture_output=True, text=True, timeout=65)
                assert result.returncode == 0, (runtime, level, whole, result.returncode, result.stdout, result.stderr)
                checks += 1
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true', help='also link the hosted program and run it under Wine')
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    with tempfile.TemporaryDirectory(prefix='dcc-seh-') as temporary:
        scratch = Path(temporary)
        checks = check_hosted_objects(dcc, scratch)
        checks += check_freestanding(dcc, scratch)
        checks += check_diagnostics(dcc, scratch)
        if args.windows:
            checks += execute_hosted(dcc, scratch)
    print(f'PASS {checks} structured exception handling checks' + ('' if args.windows else ' (execution requires --windows)'))


if __name__ == '__main__':
    main()
