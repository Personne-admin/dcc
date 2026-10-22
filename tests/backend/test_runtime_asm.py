#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import re
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]


def audit_sources():
    linux = (ROOT / 'tests/backend/runtime_linux.dc').read_text()
    decoded = '\n'.join(line.strip()[2:].replace('%%', '%') for line in linux.splitlines() if line.strip().startswith('\\\\'))
    expected = (ROOT / 'libdcext/linux/syscall.asm').read_text().replace('__syscall', 'runtime_syscall').replace('__thread_entry', 'runtime_thread_entry').replace('thread_child_main', 'runtime_child_main')
    assert expected.strip() in re.sub(r'\.section \.text\.runtime, "(?:ax|xr)"', '.text', decoded)
    windows = (ROOT / 'tests/backend/runtime_windows.dc').read_text()
    decoded = '\n'.join(line.strip()[2:].replace('%%', '%') for line in windows.splitlines() if line.strip().startswith('\\\\'))
    expected = (ROOT / 'libdcext/windows/runtime.asm').read_text().replace('test %rax, (%rcx)', 'cmp (%rcx), %rax')
    assert expected.strip() in re.sub(r'\.section \.text\.runtime, "(?:ax|xr)"', '.text', decoded)


def flags(dcc, windows, backend, opt, whole):
    return [dcc, '-target', 'x86_64-coff' if windows else 'x86_64-elf', '-fbackend', backend, '-' + opt,
            '-I', ROOT / 'libdcext', *(['-flibdcext', 'linux'] if not windows else []),
            *(['-fwhole-program', '-e', 'runtime_entry' if windows else 'main'] if whole else [])]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    audit_sources()
    source = ROOT / 'tests/backend' / ('runtime_windows.dc' if args.windows else 'runtime_linux.dc')
    with tempfile.TemporaryDirectory(prefix='dcc-runtime-asm-') as temp:
        scratch = Path(temp)
        for backend in ('llvm', 'custom'):
            for opt in ('O0', 'O2'):
                for whole in (False, True):
                    compile_flags = flags(dcc, args.windows, backend, opt, whole)
                    obj, exe = scratch / 'runtime.o', scratch / 'runtime.exe'
                    run([*compile_flags, '-c', source, '-o', obj])
                    if backend == 'custom':
                        listing, rebuilt = scratch / 'runtime.s', scratch / 'rebuilt.o'
                        run([*compile_flags, '-S', source, '-o', listing])
                        run(['nasm', '-f', 'win64' if args.windows else 'elf64', listing, '-o', rebuilt])
                        first, second = scratch / 'direct.text', scratch / 'listing.text'
                        for item, output in ((obj, first), (rebuilt, second)):
                            run(['objcopy', '-O', 'binary', '--only-section=.text.runtime', item, output])
                        direct, printed = bytearray(first.read_bytes()), bytearray(second.read_bytes())
                        for item in (obj, rebuilt):
                            active = False
                            for line in run(['objdump', '-r', item]).splitlines():
                                if line.startswith('RELOCATION RECORDS FOR '):
                                    active = line == 'RELOCATION RECORDS FOR [.text.runtime]:'
                                match = re.match(r'^([0-9a-f]+)\s+(\S+)\s+', line) if active else None
                                if match:
                                    assert match[2] in ('R_X86_64_PLT32', 'R_X86_64_PC32', 'IMAGE_REL_AMD64_REL32')
                                    offset = int(match[1], 16)
                                    direct[offset:offset + 4] = printed[offset:offset + 4] = b'\0' * 4
                        assert direct == printed
                    for executable_object in ((obj, rebuilt) if backend == 'custom' else (obj,)):
                        if args.windows:
                            dis = run(['objdump', '-dr', executable_object])
                            assert '__chkstk' in dis and '__imp_ExitProcess' in dis and 'int3' in dis
                            mingw = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw'))
                            run([mingw / 'bin/x86_64-w64-mingw32-clang', '-nostdlib', '-Wl,--entry,_start',
                                 '-Wl,--subsystem,console', executable_object, '-lkernel32', '-o', exe])
                            run([os.environ.get('WINE', 'wine'), exe])
                        else:
                            run([dcc, '-flibdcext', 'linux', '-fbackend', backend, executable_object, '-o', exe])
                            assert run([exe]) == 'runtime asm ok\n'
                    print('  PASS runtime module asm', 'windows' if args.windows else 'linux', backend,
                          opt, 'whole' if whole else 'separate', flush=True)
    print('  RESULT runtime module asm execution and listing round trips passed')


if __name__ == '__main__':
    main()
