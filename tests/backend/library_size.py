#!/usr/bin/env python3
from pathlib import Path
import argparse
import concurrent.futures
import csv
import json
import os
import re
import shutil
import subprocess
import struct
import sys
from process_output import run_output

ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = ROOT / 'tests/stdlib/library-size'


def run(command, cwd=None, limit=65):
    result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, text=True, timeout=limit)
    if result.returncode:
        raise RuntimeError(str(command) + '\n' + result.stdout + result.stderr)
    return result.stdout


def compile_dc(command, cwd=None):
    return run(['timeout', '60', *command], cwd)


def build_archive(dcc, out, variant, backend, opt, no_float):
    build = out / 'archives' / (variant + '-' + backend + '-' + opt + ('-no-float' if no_float else '-float'))
    run(['make', '-s', '--no-print-directory', '-j4', '-C', ROOT / 'libdcext',
         'RULES_MK=' + str(ROOT / 'mk/rules.mk'), 'TOPLEVEL=' + str(ROOT),
         'BUILD_DIR=' + str(build), 'OBJ_DIR=' + str(build / 'obj'), 'DEP_DIR=' + str(build / 'dep'),
         'NATIVE_BUILD_DIR=' + str(dcc.parent.parent), 'DCC=' + str(dcc), 'DCC_WRAPPER=timeout 60',
         'TARGET=x86_64-' + variant, 'BACKEND=' + backend, 'LIBDCEXT_OPT=' + opt,
         'LIBDCEXT_TARGET_FLAGS=' + ('-fno-simd -fno-x87' if no_float else ''),
         'FILE_PREFIX_MAP_FLAGS=-ffile-prefix-map=' + str(ROOT / 'libdcext') + '=libdcext',
         'CXX=clang++', 'CC=clang', 'AR=ar'], limit=600)
    archive = build / 'lib' / ('libdcext-' + variant + '-' + backend + '.a')
    return archive


def inspect(exe):
    image = exe.read_bytes()
    sizes = dict(text=0, rodata=0, data=0, bss=0)
    records = []
    if image.startswith(b'MZ'):
        header = struct.unpack_from('<I', image, 0x3c)[0] + 4
        count = struct.unpack_from('<H', image, header + 2)[0]
        table = header + 20 + struct.unpack_from('<H', image, header + 16)[0]
        for i in range(count):
            section = table + i * 40
            name = image[section:section + 8].split(b'\0')[0].decode()
            virtual_size, _, raw_size = struct.unpack_from('<III', image, section + 8)
            flags = struct.unpack_from('<I', image, section + 36)[0]
            if flags & 0x02000000 or not flags & 0xe0000000:
                continue
            size = virtual_size or raw_size
            records.append((name, size, flags))
            initialized = min(size, raw_size)
            sizes['bss'] += size - initialized
            kind = 'text' if flags & 0x20 else 'data' if flags & 0x80000000 else 'rodata'
            sizes[kind] += initialized
    else:
        sections = run(['objdump', '-h', exe])
        lines = sections.splitlines()
        for i, line in enumerate(lines):
            match = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+', line)
            if match:
                flags = lines[i + 1] if i + 1 < len(lines) else ''
                if 'ALLOC' in flags:
                    records.append((match[1], int(match[2], 16), flags))
        for name, size, flags in records:
            if 'CODE' in flags:
                kind = 'text'
            elif 'CONTENTS' not in flags:
                kind = 'bss'
            elif 'READONLY' in flags:
                kind = 'rodata'
            else:
                kind = 'data'
            sizes[kind] += size
    symbols = run(['nm', '--defined-only', exe])
    disassembly = run(['objdump', '-d', '--insn-width=16', exe])
    startup_bytes = startup_instructions = 0
    active = False
    for line in disassembly.splitlines():
        label = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if label:
            active = label[1] in ('_start', '__dc_rt_init', '__dc_main')
        instruction = re.match(r'\s*[0-9a-f]+:\s+((?:[0-9a-f]{2}\s+)+)\s*\S', line)
        if active and instruction:
            startup_bytes += len(instruction[1].split())
            startup_instructions += 1
    return {**sizes, 'file_bytes': exe.stat().st_size, 'loadable_sections': len(records),
            'retained_symbols': len(symbols.splitlines()), 'startup_bytes': startup_bytes,
            'startup_instructions': startup_instructions,
            'has_json': bool(re.search(r'json', symbols)),
            'has_network': bool(re.search(r'\.net\b|TcpStream|TcpListener', symbols)),
            'has_thread_entry': bool(re.search(r'\.thread\b|__thread_entry|thread_child_main', symbols))}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--windows', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--opts', nargs='+', default=['O2', 'Os'])
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    variant, target = ('windows', 'x86_64-coff') if args.windows else ('linux', 'x86_64-elf')
    prefix = out / 'source-only'
    (prefix / 'bin').mkdir(parents=True, exist_ok=True)
    compiler = prefix / 'bin/dcc'
    if compiler.exists():
        compiler.unlink()
    try:
        os.link(dcc, compiler)
    except OSError:
        shutil.copy2(dcc, compiler)
    if (prefix / 'include').exists():
        shutil.rmtree(prefix / 'include')
    shutil.copytree(dcc.parent.parent / 'include', prefix / 'include')
    assert not (prefix / 'lib').exists()
    archives = {}
    for backend in ('llvm', 'custom'):
        for opt in args.opts:
            for no_float in (False, True):
                archive = build_archive(dcc, out, variant, backend, opt, no_float)
                archives[backend, opt, no_float] = archive
                print('built', variant, backend, opt, 'no-float' if no_float else 'float', archive.stat().st_size, flush=True)
    if args.windows:
        cc = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang'
    tasks = [(program, backend, opt, no_float, whole, gc) for program in sorted(PROGRAMS.glob('*.dc'))
             for backend in ('llvm', 'custom') for opt in args.opts for no_float in (False, True)
             for whole in (False, True) for gc in (False, True)]

    def measure(task):
        program, backend, opt, no_float, whole, gc = task
        key = '-'.join([program.stem, backend, opt, 'no-float' if no_float else 'float', 'whole' if whole else 'separate', 'gc' if gc else 'no-gc'])
        work = out / key
        work.mkdir(exist_ok=True)
        obj, exe = work / 'app.o', work / ('app.exe' if args.windows else 'app')
        mapping = ['-ffile-prefix-map=' + str(ROOT / 'libdcext') + '=libdcext',
                   '-ffile-prefix-map=' + str(dcc.parent.parent / 'include') + '=libdcext',
                   '-ffile-prefix-map=' + str(prefix / 'include') + '=libdcext',
                   '-ffile-prefix-map=' + str(ROOT) + '=dcc', '-ffile-prefix-map=' + str(out) + '=size-gate']
        flags = ['-target', target, '-flibdcext', variant, '-fbackend', backend, '-' + opt,
                 *(['-fno-simd', '-fno-x87'] if no_float else []), *mapping]
        reachability = compile_dc([compiler if whole else dcc, *flags, *(['-fwhole-program', '--print-reachability'] if whole else []),
                                   '-c', program, '-o', obj])
        (work / 'reachability.log').write_text(reachability)
        archive = archives[backend, opt, no_float]
        link_map = work / 'link.map'
        if args.windows:
            command = [cc, '-nostdlib', '-Wl,--entry,_start', '-Wl,--undefined,_start', '-Wl,--subsystem,console',
                       '-Wl,--gc-sections' if gc else '-Wl,--no-gc-sections', '-Wl,--no-insert-timestamp', '-Wl,-Map,' + str(link_map),
                       obj, *([] if whole else [archive]), '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe]
        else:
            command = ['ld.lld', '--static', '--no-dynamic-linker', '--fatal-warnings', '--undefined=_start',
                       '--gc-sections' if gc else '--no-gc-sections', '-Map=' + str(link_map), obj,
                       *([] if whole else [archive]), '-o', exe]
        if not whole:
            command = [dcc, '-target', target, '-flibdcext', variant, '-fbackend', backend,
                       '-L', archive.parent, obj, '--gc-sections' if gc else '--no-gc-sections',
                       '-Wl,-Map,' + str(link_map), '--print-reachability',
                       *(['-L', Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'x86_64-w64-mingw32/lib',
                          '-Wl,--no-insert-timestamp', '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32'] if args.windows else []), '-o', exe]
        linked = run(command)
        (work / 'link-reachability.log').write_text(linked)
        stdout = run_output(['timeout', '60', *([os.environ.get('WINE', 'wine')] if args.windows else []), exe], cwd=work)
        expected = {'hello': 'hello\n', 'integer': '12345:-17\n'}.get(program.stem, '')
        assert stdout == expected, (key, repr(stdout), repr(expected))
        data = inspect(exe)
        if whole and program.stem in ('empty', 'hello', 'integer'):
            assert not data['has_json'] and not data['has_network'] and not data['has_thread_entry'], (key, data)
        return {'program': program.stem, 'os': variant, 'backend': backend, 'opt': opt,
                'float': not no_float, 'mode': 'whole' if whole else 'separate', 'gc': gc,
                'archive_bytes': 0 if whole else archive.stat().st_size, 'archive_product_bytes': archive.stat().st_size,
                'provider_comparison': 'pending step 12; paired measurement in step 26', **data}

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(measure, task) for task in tasks]
        try:
            for future in concurrent.futures.as_completed(futures):
                row = future.result()
                rows.append(row)
                print('PASS', row['program'], row['backend'], row['opt'], row['float'], row['mode'], row['gc'], row['file_bytes'], flush=True)
        except BaseException:
            for future in futures:
                future.cancel()
            raise
    rows.sort(key=lambda row: (row['program'], row['backend'], row['opt'], row['float'], row['mode'], row['gc']))
    payload = {'revision': run(['git', '-C', ROOT, 'rev-parse', 'HEAD']).strip(), 'toolchain': compile_dc([dcc, '--version']).strip(),
               'linker': run(['ld.lld', '--version']).strip(), 'llvm': run(['llvm-config', '--version']).strip(),
               'flags': '-O2/-Os; default float or -fno-simd -fno-x87; --gc-sections/--no-gc-sections; source paths normalized to libdcext/dcc', 'debug': 'none; symbols retained identically',
               'section_size_definition': 'PE uses virtual sizes and raw-data sizes, including zero-filled virtual tails; ELF uses allocated sections including zero-fill RELRO padding; discardable PE relocation/debug sections excluded',
               'startup_cost_definition': 'static code bytes and instructions in _start, __dc_rt_init and __dc_main; excludes application main and OS loader',
               'provider_comparison': 'pending step 12; required at step 26', 'rows': rows}
    (out / 'measurements.json').write_text(json.dumps(payload, indent=2, sort_keys=True) + '\n')
    with (out / 'measurements.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print('RESULT', len(rows), 'code-size/behavior measurements', variant, flush=True)


if __name__ == '__main__':
    main()
