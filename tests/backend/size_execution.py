from pathlib import Path
import os
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]

def audit_object(obj, target, integer_helpers=False):
    undefined = run(['nm', '-u', obj])
    allowed = {'memcpy', 'memmove', 'memset', 'memcmp'}
    if integer_helpers:
        if target == 'x86-elf':
            allowed.update(('__udivdi3', '__umoddi3'))
        elif target == 'x86-coff':
            allowed.update(('__aulldiv', '__aullrem'))
    if target == 'x86-coff':
        allowed.update('_' + name for name in ('memcpy', 'memmove', 'memset', 'memcmp'))
    if target.endswith('coff'):
        allowed.add('__chkstk')
    for symbol in undefined.splitlines():
        assert symbol.split()[-1] in allowed, undefined


def native(dcc, scratch, windows, source):
    target = 'x86_64-coff' if windows else 'x86_64-elf'
    variant = 'windows' if windows else 'linux'
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            for whole in (False, True):
                obj, exe = scratch / 'test.o', scratch / 'test.exe'
                flags = [dcc, '-flibdcext', variant, '-target', target, '-fbackend', backend, '-' + opt,
                         '-I', ROOT / 'libdcext']
                if whole:
                    flags += ['-fwhole-program']
                if windows:
                    run([*flags, '-c', source, '-o', obj])
                    mingw = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw'))
                    archives = [] if whole else [dcc.parent.parent / 'lib' / ('libdcext-' + variant + '-' + backend + '.a')]
                    run([mingw / 'bin/x86_64-w64-mingw32-clang', '-nostdlib', '-Wl,--entry,_start',
                         '-Wl,--subsystem,console', obj, *archives,
                         '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe])
                    run([os.environ.get('WINE', 'wine'), exe])
                else:
                    if whole:
                        run([*flags, source, '-o', exe])
                    else:
                        run([*flags, '-c', source, '-o', obj])
                        run([dcc, '-flibdcext', variant, '-fbackend', backend, obj, '-o', exe])
                    run([exe])
                print('  PASS size arithmetic', variant, backend, opt, 'whole' if whole else 'separate', flush=True)


def portable(dcc, scratch, fixture):
    original = fixture.read_text()
    source = scratch / 'portable.dc'
    source.write_text(original.replace('public i32 main() { return dcc_main(); }', ''))
    for target in ('x86-elf', 'x86-coff', 'x86_64-elf', 'x86_64-coff'):
        backends = ('llvm', 'custom') if target.startswith('x86_64') else ('llvm',)
        for backend in backends:
            for opt in ('O0', 'O2'):
                obj = scratch / 'portable.o'
                run([dcc, '-target', target, '-fbackend', backend, '-' + opt, '-I', ROOT / 'libdcext',
                     '-fwhole-program', '-e', 'dcc_main', '-fno-simd', '-fno-x87', '-c', source, '-o', obj])
                audit_object(obj, target, fixture is not None and fixture.name == 'size-format.dc')
    for model in ('small', 'unreal', 'unreal32'):
        startup = ROOT / 'tests/i8086/crt0.dc'
        assert startup.exists(), startup
        entry = scratch / 'entry.dc'
        entry.write_text(source.read_text() + '\n' + startup.read_text())
        for opt in ('O0', 'O2'):
            binary = scratch / 'portable.bin'
            run([dcc, '-target', 'i8086-binary', '-fbackend', 'custom', '-mcmodel=' + model, '-' + opt, '-I', ROOT / 'libdcext',
                 '-e', '_start', entry, '-o', binary])
            run(['python3', ROOT / 'tests/i8086/run.py', binary, '--mode', 'unreal' if model != 'small' else 'real'])
            print('  PASS size arithmetic i8086', model, opt, flush=True)

