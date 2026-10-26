#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]


def error(command, *messages):
    result = subprocess.run(['timeout', '60', *map(str, command)], capture_output=True, text=True, timeout=65)
    assert result.returncode == 1, (command, result.returncode, result.stderr)
    for message in messages:
        assert message in result.stderr, (message, result.stderr)


def execute(exe, windows):
    run([*([os.environ.get('WINE', 'wine')] if windows else []), exe, 'argument'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    variant, target = ('windows', 'x86_64-coff') if args.windows else ('linux', 'x86_64-elf')
    system = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'x86_64-w64-mingw32/lib'
    system_flags = ['-L', system, '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32'] if args.windows else []
    manifest = (ROOT / 'libdcext/libdcext.manifest').read_text().splitlines()
    profiles = ('all', 'hosted', variant)
    expected = [Path(row.split()[3]).name + '.o' for row in manifest if row.split()[0] in ('source', 'runtime') and row.split()[1] in profiles]
    with tempfile.TemporaryDirectory(prefix='dcc-library-archive-') as temporary:
        root = Path(temporary)
        source, other = root / 'main.dc', root / 'other.dc'
        for backend in ('llvm', 'custom'):
            archive = dcc.parent.parent / 'lib' / ('libdcext-' + variant + '-' + backend + '.a')
            members = run(['ar', 't', archive]).splitlines()
            assert sorted(members) == sorted(expected), (members, expected)
            process = root / 'process.o'
            process.write_bytes(subprocess.check_output(['ar', 'p', archive, 'process.dc.o'], timeout=60))
            sections = run(['objdump', '-h', process])
            assert len(re.findall(r'\.text[.$]', sections)) > 1, sections
            for opt in ('O0', 'O2'):
                compile_flags = [dcc, '-target', target, '-fbackend', backend, '-' + opt, '-ffunction-sections', '-fdata-sections']
                link = [dcc, '-target', target, '-flibdcext', variant, '-fbackend', backend, *system_flags]
                obj, extra, exe = root / 'main.o', root / 'extra.o', root / ('app.exe' if args.windows else 'app')
                for with_library in (False, True):
                    for takes_args in (False, True):
                        parameters = '[] const char* args' if takes_args else ''
                        check = f'if args.len != {0 if args.windows else 2} {{ return 1; }}' if takes_args else ''
                        source.write_text(f'module ordinary_application; public i32 main({parameters}) {{ {check} return 0; }}\n'
                                          '@nomangle public i32 unused_function() { return 17; }\n@nomangle public i32 unused_data = 31;\n')
                        run([*compile_flags, *(['-flibdcext', variant] if with_library else []), '-c', source, '-o', obj])
                        definitions = run(['nm', '--defined-only', obj])
                        assert '__dc_main' not in definitions and 'unused_function' in definitions
                        for control in ([], ['--gc-sections'], ['--no-gc-sections'], ['-Wl,--no-gc-sections']):
                            trace = run([*link, '--print-reachability', *control, obj, '-o', exe])
                            assert 'final link main' in trace, trace
                            execute(exe, args.windows)
                            symbols = run(['nm', '--defined-only', exe])
                            collected = not control or control == ['--gc-sections']
                            assert ('unused_function' not in symbols) == collected, (control, symbols)
                            assert ('unused_data' not in symbols) == collected, (control, symbols)
                            assert '_start' in symbols and '__dc_main' in symbols
                            assert '__dcc_libdcext_abi_requires_float_enabled' in symbols
                        whole_obj = root / 'whole.o'
                        run([*compile_flags, '-flibdcext', variant, '-fwhole-program', '-c', source, '-o', whole_obj])
                        run([*link, '-fwhole-program', whole_obj, '-o', exe])
                        execute(exe, args.windows)
                source.write_text('module no_main; public i32 other() { return 0; }\n')
                run([*compile_flags, '-c', source, '-o', obj])
                error([*link, obj, '-o', exe], 'missing program main', 'i32 main()', 'i32 main([] const char*)')
                source.write_text('module first; public i32 main() { return 0; }\n')
                other.write_text('module second; public i32 main([] const char* args) { return args.len as i32; }\n')
                run([*compile_flags, '-c', source, '-o', obj])
                run([*compile_flags, '-c', other, '-o', extra])
                error([*link, obj, extra, '-o', exe], 'ambiguous program main', 'main.dc:1', 'other.dc:1', 'i32 main()', 'i32 main([] const char*)')
                other.write_text('module second; public i32 main() { return 0; }\n')
                run([*compile_flags, '-c', other, '-o', extra])
                error([*link, obj, extra, '-o', exe], 'ambiguous program main', 'main.dc:1', 'other.dc:1')
                for parameters in ('', '[] const char* args'):
                    check = f'if args.len != {0 if args.windows else 2} {{ return 1; }}' if parameters else ''
                    source.write_text(f'module native_main; @nomangle public i32 main({parameters}) {{ {check} return 0; }}\n')
                    run([*compile_flags, '-c', source, '-o', obj])
                    run([*link, obj, '-o', exe])
                    execute(exe, args.windows)
                mismatch = root / 'mismatch.o'
                run([*compile_flags, '-target', 'x86_64-elf' if args.windows else 'x86_64-coff', '-c', source, '-o', mismatch])
                error([*link, mismatch, '-o', exe], 'incompatible', 'not an x86_64 ' + ('COFF' if args.windows else 'ELF'))
                mismatch.write_bytes(b'short')
                error([*link, mismatch, '-o', exe], 'not an x86_64')
                damaged = root / 'damaged.o'
                data = obj.read_bytes()
                assert b'DCCMAIN1:' in data
                damaged.write_bytes(data.replace(b'DCCMAIN1:', b'DCCMAIN0:', 1))
                error([*link, damaged, '-o', exe], 'invalid main metadata')
                print('  PASS archive sections, final main and GC controls', variant, backend, opt, flush=True)
    print('  RESULT source/archive inventory parity, compile-flag-independent final main and section collection passed')


if __name__ == '__main__':
    main()
