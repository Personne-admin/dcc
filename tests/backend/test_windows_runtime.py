#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]
SYMBOLS = {'startup': '_start', 'chkstk': '__chkstk', 'fltused': '_fltused'}


def decoded(source):
    return '\n'.join(line.strip()[2:].replace('%%', '%') for line in source.splitlines() if line.strip().startswith('\\\\'))


def check_leaf(obj, name):
    symbols = run(['nm', '-g', '--defined-only', obj])
    assert re.findall(r'\b[TD] (\S+)$', symbols, re.M) == [SYMBOLS[name]], symbols
    undefined = set(re.findall(r'\bU (\S+)$', run(['nm', '-u', obj]), re.M))
    expected = {'__dc_rt_init', '__dc_main', '__imp_ExitProcess'} if name == 'startup' else set()
    assert undefined == expected, (name, undefined)
    relocations = run(['objdump', '-r', obj])
    dis = run(['objdump', '-dr', obj])
    if name == 'startup':
        for symbol in expected:
            assert re.search(r'IMAGE_REL_AMD64_REL32\s+' + symbol + r'\b', relocations), relocations
        assert 'int3' in dis and not re.search(r'_DC0F|GOT|WEAK', dis + symbols)
    elif name == 'chkstk':
        assert not re.search(r'\bcall\w*\b|%(?:xmm|ymm|zmm|mm)\d+', dis), dis
        assert len(re.findall(r'\bcmp\s+\(%rcx\),%rax', dis)) == 2, dis
    else:
        data = obj.with_suffix('.bin')
        run(['objcopy', '-O', 'binary', '--only-section=.data.windows.fltused', obj, data])
        assert data.read_bytes() == b'\0' * 4


def check_archive(archive, scratch):
    members = run(['ar', 't', archive]).splitlines()
    assert not any(name.endswith('.asm.o') for name in members), members
    for name in SYMBOLS:
        member = name + '.dc.o'
        assert members.count(member) == 1, (archive, member)
        obj = scratch / member
        obj.write_bytes(subprocess.check_output(['ar', 'p', archive, member], timeout=60))
        check_leaf(obj, name)


def no_float_library(dcc, scratch, backend):
    build = scratch / ('no-float-' + backend)
    run(['make', '--no-print-directory', '-s', '-j4', '-C', ROOT / 'libdcext',
         'RULES_MK=' + str(ROOT / 'mk/rules.mk'), 'TOPLEVEL=' + str(ROOT),
         'BUILD_DIR=' + str(build), 'OBJ_DIR=' + str(build / 'obj'), 'DEP_DIR=' + str(build / 'dep'),
         'NATIVE_BUILD_DIR=' + str(dcc.parent.parent), 'DCC=' + str(dcc), 'DCC_WRAPPER=timeout 60',
         'TARGET=x86_64-windows', 'BACKEND=' + backend, 'LIBDCEXT_TARGET_FLAGS=-fno-simd -fno-x87',
         'AR=ar', 'CXX=clang++', 'CC=clang', 'Q=@'])
    for prefix in ('libdcext-',):
        check_archive(build / 'lib' / (prefix + 'windows-' + backend + '.a'), scratch)
    return build / 'lib'


def application():
    source = (ROOT / 'tests/backend/runtime_windows.dc').read_text()
    prefix = source.split('asm {', 1)[0].replace('@nomangle public i32 runtime_entry()', 'public i32 main()')
    probe = source[source.index('    \\\\.globl runtime_probe_check'):source.index('    \\\\.globl _start')]
    return prefix + 'asm {\n    \\\\.section .text.production_probe, "xr"\n' + probe + '};\n'


def execute(exe, expected):
    result = subprocess.run(['timeout', '60', os.environ.get('WINE', 'wine'), str(exe)], capture_output=True, text=True, timeout=65)
    assert result.returncode == expected, (exe, result.returncode, result.stdout, result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    assert not list((ROOT / 'libdcext').rglob('*.asm'))
    makefile = (ROOT / 'libdcext/GNUmakefile').read_text()
    assert not re.search(r'ASM_CC|ASM_TARGET|ASM_OBJS|ASM_SRCS|assembler-with-cpp|MINGW', makefile)
    with tempfile.TemporaryDirectory(prefix='dcc-windows-runtime-') as temporary:
        scratch = Path(temporary)
        source = scratch / 'application.dc'
        contract = ROOT / 'tests/backend/windows_startup_contract.dc'
        for backend in ('llvm', 'custom'):
            if args.windows:
                for prefix in ('libdcext-',):
                    check_archive(dcc.parent.parent / 'lib' / (prefix + 'windows-' + backend + '.a'), scratch)
            no_float_lib = no_float_library(dcc, scratch, backend) if args.windows else None
            reference = {}
            for opt in ('O0', 'O2'):
                objects = {}
                for name in SYMBOLS:
                    obj = scratch / (name + '.dc.o')
                    run([dcc, '-target', 'x86_64-coff', '-fbackend', backend, '-' + opt, '-c', ROOT / 'libdcext/windows' / (name + '.dc'), '-o', obj])
                    check_leaf(obj, name)
                    section = '.data.windows.fltused' if name == 'fltused' else '.text.windows.' + name
                    data = scratch / 'leaf.bin'
                    run(['objcopy', '-O', 'binary', '--only-section=' + section, obj, data])
                    assert data.stat().st_size > 0
                    if name in reference:
                        assert data.read_bytes() == reference[name], (backend, name, 'O0/O2 differ')
                    reference[name] = data.read_bytes()
                    objects[name] = obj
                for whole in (False, True):
                    mode = ['-fwhole-program'] if whole else []
                    common = [dcc, '-target', 'x86_64-coff', '-fbackend', backend, '-' + opt]
                    obj, exe = scratch / 'app.o', scratch / 'app.exe'
                    run([*common, *mode, '-c', contract, '-o', obj])
                    if args.windows:
                        mingw = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw'))
                        run([mingw / 'bin/x86_64-w64-mingw32-clang', '-nostdlib', '-Wl,--entry,_start', '-Wl,--subsystem,console', obj, objects['startup'], '-lkernel32', '-o', exe])
                        execute(exe, 37)
                    for no_float in (False, True):
                        flags = [*common, '-flibdcext', 'windows', *mode, *(['-fno-simd', '-fno-x87'] if no_float else [])]
                        for takes_args in (False, True):
                            body = application()
                            if takes_args:
                                body = body.replace('public i32 main() {', 'public i32 main([] const char* args) {\n    if args.len != 0 { return 3; }')
                            if not no_float:
                                body = body.replace('public i32 main(', '@runtime @noinline public f64 float_control(f64 value) { return value + 1.5; }\n@nomangle extern i32 runtime_float_marker();\npublic i32 main(')
                                body = body.replace('    if runtime_probe_check', '    if float_control(2.5) != 4.0 || runtime_float_marker() != 0 { return 4; }\n    if runtime_probe_check', 1)
                                body += 'asm { ".section .text.float_marker, \\"xr\\"\\n.globl runtime_float_marker\\nruntime_float_marker:\\nmov _fltused(%%rip), %%eax\\nret\\n" };\n'
                            source.write_text(body)
                            run([*flags, '-c', source, '-o', obj])
                            undefined = run(['nm', '-u', obj])
                            defined = run(['nm', '-g', '--defined-only', obj])
                            assert bool(re.search(r'\b_fltused$', undefined, re.M)) == (not no_float and not whole), undefined
                            assert bool(re.search(r'\b_fltused$', defined, re.M)) == (not no_float and whole), defined
                            if args.windows:
                                library = no_float_lib if no_float else dcc.parent.parent / 'lib'
                                archives = [] if whole else [library / ('libdcext-windows-' + backend + '.a')]
                                run([mingw / 'bin/x86_64-w64-mingw32-clang', '-nostdlib', '-Wl,--entry,_start',
                                     '-Wl,--undefined,_start', '-Wl,--subsystem,console', obj, *archives,
                                     '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe])
                                execute(exe, 0)
                    print('  PASS Windows runtime', backend, opt, 'whole' if whole else 'separate', 'execution' if args.windows else 'COFF', flush=True)
    print('  RESULT Windows DC runtime, startup contract, production probe, both mains and float profiles passed')


if __name__ == '__main__':
    main()
