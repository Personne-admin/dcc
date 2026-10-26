#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]
NAMES = {'startup': '_start', **{f'syscall{i}': f'__syscall{i}' for i in range(7)}, 'clone': '__thread_entry'}


def check_leaf(obj, name):
    definitions = run(['nm', '-g', '--defined-only', obj])
    assert re.findall(r'\bT (\S+)$', definitions, re.M) == [NAMES[name]], definitions
    undefined = set(re.findall(r'\bU (\S+)$', run(['nm', '-u', obj]), re.M))
    expected = {'__dc_rt_init', '__dc_main'} if name == 'startup' else {'thread_child_main'} if name == 'clone' else set()
    assert undefined == expected, (name, undefined)
    dis = run(['objdump', '-dr', obj])
    assert not re.search(r'@GOT|_DC0F|\bWEAK\b', run(['readelf', '-Wr', '-Ws', obj]))
    assert len(re.findall(r'\bsyscall\b', dis)) == (2 if name == 'clone' else 1), dis
    if name.startswith('syscall'):
        assert not re.search(r'\bcall\w*\b', dis), dis


def check_archive(archive, scratch):
    members = run(['ar', 't', archive]).splitlines()
    assert 'crt0.asm.o' not in members and 'syscall.asm.o' not in members, members
    for name in NAMES:
        member = name + '.dc.o'
        assert members.count(member) == 1, (archive, member)
        obj = scratch / member
        obj.write_bytes(subprocess.check_output(['ar', 'p', archive, member], timeout=60))
        check_leaf(obj, name)


def execute(exe, arguments=(), expected=0):
    result = subprocess.run(['timeout', '60', str(exe), *arguments], capture_output=True, text=True,
                            env={**os.environ, 'DCC_STARTUP_TEST': 'environment value'}, timeout=65)
    assert result.returncode == expected, (exe, result.returncode, result.stdout, result.stderr)
    return result.stdout


def direct_source():
    fixture = (ROOT / 'tests/backend/runtime_linux.dc').read_text()
    prefix = fixture.split('asm {', 1)[0].replace('import std::slice;', '')
    prefix = prefix.replace('public i32 main()', '@nomangle public i32 production_runtime_test()')
    prefix = prefix.replace('runtime_syscall', '__syscall').replace('runtime_thread_entry', '__thread_entry').replace('runtime_child_main', 'thread_child_main')
    probe = fixture[fixture.index('    \\\\.globl runtime_clobbers'):]
    return prefix + 'asm {\n    \\\\.section .text.production_probe, "ax"\n' + probe + '''
asm {
    \\\\.text
    \\\\.globl _start
    \\\\_start:
    \\\\and $-16, %%rsp
    \\\\call production_runtime_test
    \\\\mov %%eax, %%edi
    \\\\mov $231, %%eax
    \\\\syscall
    \\\\ud2
    \\\\.section .note.GNU-stack,"",@progbits
    \\\\
};
'''


def main():
    dcc = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='dcc-linux-runtime-') as temporary:
        scratch = Path(temporary)
        source = scratch / 'startup.dc'
        direct = scratch / 'direct.dc'
        direct.write_text(direct_source())
        startup = (ROOT / 'tests/backend/linux_startup.dc').read_text()
        for backend in ('llvm', 'custom'):
            lib = dcc.parent.parent / 'lib'
            for prefix in ('libdcext-',):
                check_archive(lib / (prefix + 'linux-' + backend + '.a'), scratch)
            for opt in ('O0', 'O2'):
                objects = []
                for name in NAMES:
                    obj = scratch / (name + '.dc.o')
                    run([dcc, '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt, '-c',
                         ROOT / 'libdcext/linux' / (name + '.dc'), '-o', obj])
                    check_leaf(obj, name)
                    if name != 'startup':
                        objects.append(obj)
                for whole in (False, True):
                    flags = [dcc, '-target', 'x86_64-elf', '-flibdcext', 'linux', '-fbackend', backend, '-' + opt,
                             *(['-fwhole-program'] if whole else [])]
                    for takes_args in (False, True):
                        body = startup.replace('public i32 main() { return check_startup(); }',
                            'public i32 main([] const char* args) { if args.len != 3 || args[1][0] != \'h\' || args[2][0] != \'\\0\' { return 7; } return check_startup(); }') if takes_args else startup
                        source.write_text(body)
                        obj, exe = scratch / 'app.o', scratch / 'app'
                        if whole:
                            run([*flags, source, '-o', exe])
                        else:
                            run([*flags, '-c', source, '-o', obj])
                            run([dcc, '-flibdcext', 'linux', '-fbackend', backend, obj, '-o', exe])
                        execute(exe, ('hello space', ''))
                    obj, exe = scratch / 'direct.o', scratch / 'direct'
                    run([dcc, '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt,
                         *(['-fwhole-program', '-e', 'production_runtime_test'] if whole else []), '-c', direct, '-o', obj])
                    run(['ld.lld', '--static', '--fatal-warnings', '--entry=_start', obj, *objects,
                         lib / ('libdcext-freestanding-' + backend + '.a'), '-o', exe])
                    assert execute(exe) == 'runtime asm ok\n'
                    exit_source = ROOT / 'tests/stdlib/process-exit-with-thread.dc'
                    if whole:
                        run([*flags, exit_source, '-o', exe])
                    else:
                        run([*flags, '-c', exit_source, '-o', obj])
                        run([dcc, '-flibdcext', 'linux', '-fbackend', backend, obj, '-o', exe])
                    execute(exe, expected=37)
                    print('  PASS Linux startup/syscalls/clone', backend, opt, 'whole' if whole else 'separate', flush=True)
    print('  RESULT Linux native runtime, argc/argv/env, alignment, both mains and process/thread exits passed')


if __name__ == '__main__':
    main()
