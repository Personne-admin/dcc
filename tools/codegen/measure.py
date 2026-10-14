#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tests/i8086'))
from run import build_image
CONFIGS = ('small', 'unreal', 'unreal32', 'em64t', 'llvm')
CATEGORIES = ('spill_stores', 'spill_reloads', 'register_copies', 'frame_setup', 'branches', 'calls', 'everything_else')


def run(args, cwd=None, timeout=60):
    env = dict(os.environ, DCC_MEASURE_ASM_VERBOSE='1')
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env)
    if result.returncode:
        raise RuntimeError(f'{shlex.join(list(map(str, args)))}\n{result.stdout}\n{result.stderr}')
    return result.stdout


def corpus(destination):
    entries = []
    for line in (ROOT / 'tests/i8086/differential.list').read_text().splitlines():
        name = line.split('#')[0].strip()
        if not name:
            continue
        fixture = ROOT / 'tests/cases' / name
        text = fixture.read_text()
        files = re.findall(r'^=== FILE: (.*?) ===\n(.*?)(?=^=== |\Z)', text, re.M | re.S)
        entry = re.search(r'^=== ENTRY: (.*?) ===', text, re.M)
        directory = destination / Path(name).stem
        directory.mkdir(parents=True, exist_ok=True)
        for filename, source in files:
            path = directory / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        expected = int(re.search(r'^RUN-EXIT: (\d+)', text, re.M)[1])
        entries.append((Path(name).stem, directory / (entry[1] if entry else files[0][0]), expected, str(fixture)))
    for directory in sorted((HERE / 'corpus').iterdir()):
        entries.append(('realistic-' + directory.name, directory / 'main.dc', 0, str(directory)))
    return entries


def symbols(path):
    result = []
    for line in run(['readelf', '-sW', path]).splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[3] == 'FUNC' and fields[6] != 'UND' and int(fields[2]):
            result.append({'name': fields[7], 'address': int(fields[1], 16), 'size': int(fields[2])})
    return result


def disassemble(path, real=False, base=0):
    args = ['objdump', '-D' if real else '-d', '--insn-width=16', '-M', 'intel,addr16,data16' if real else 'intel']
    if real:
        args += ['-b', 'binary', '-m', 'i386', f'--adjust-vma={base}']
    text = run(args + [path])
    result = []
    for line in text.splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)', line)
        if match:
            raw = bytes.fromhex(match[2])
            result.append({'address': int(match[1], 16), 'bytes': raw.hex(), 'size': len(raw), 'asm': match[3].strip()})
    return result


def mir_frames(text):
    frames = {}
    current = None
    for line in text.splitlines():
        match = re.match(r'func (.*?) \[frame_size=(\d+)', line)
        if match:
            current = frames[match[1]] = {'frame_size': int(match[2]), 'slots': []}
        match = re.match(r'slot offset=(-?\d+) size=(\d+) spill=([01])', line)
        if match and current is not None:
            current['slots'].append({'offset': int(match[1]), 'size': int(match[2]), 'spill': bool(int(match[3]))})
    return frames


def llvm_frames(text):
    frames = {}
    current = None
    for line in text.splitlines():
        match = re.match(r'^([^\s.#][^:]*):\s*(?:#.*)?$', line)
        if match:
            current = frames.setdefault(match[1], {'frame_size': 0, 'slots': [], 'llvm_spill_refs': [], 'alignment_push_bytes': 0})
        if current is None:
            continue
        if re.search(r'\bpushq\s+%(rax|rcx|rdx|rsi|rdi|r8|r9|r10|r11)\b', line):
            current['alignment_push_bytes'] += 8
        alloc = re.search(r'\bsubq\s+\$(\d+), %rsp', line)
        if alloc:
            current['frame_size'] = max(current['frame_size'], int(alloc[1]))
        redzone = re.search(r'(-\d+)\(%rsp\)', line)
        if redzone:
            current['frame_size'] = max(current['frame_size'], -int(redzone[1]))
        if 'Spill' in line or 'Reload' in line:
            memory = re.search(r'(-?\d*)\(%(rsp|rbp)\)', line)
            if memory:
                ref = (memory[2], int(memory[1] or 0))
                if ref not in current['llvm_spill_refs']:
                    current['llvm_spill_refs'].append(ref)
                extent = -ref[1] if ref[0] == 'rsp' and ref[1] < 0 else 0
                current['frame_size'] = max(current['frame_size'], extent)
    for frame in frames.values():
        frame['frame_size'] += frame['alignment_push_bytes']
    return frames


def memory_refs(asm):
    refs = []
    for match in re.finditer(r'\[(rbp|rsp|bp|sp)(?:([+-])0x([0-9a-f]+))?\]', asm):
        refs.append((match[1], (int(match[3], 16) * (-1 if match[2] == '-' else 1)) if match[3] else 0))
    return refs


def classify(instruction, frame):
    asm = instruction['asm'].split('#')[0].strip()
    parts = asm.split(None, 1)
    mnemonic = parts[0] if parts else ''
    operands = [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []
    refs = memory_refs(asm)
    spill = any(base in ('bp', 'rbp') and slot['spill'] and slot['offset'] <= offset < slot['offset'] + slot['size']
                for base, offset in refs for slot in frame.get('slots', []))
    spill |= any((base, offset) in {tuple(ref) for ref in frame.get('llvm_spill_refs', [])} for base, offset in refs)
    if mnemonic.startswith('call'):
        category = 'calls'
    elif mnemonic.startswith('j') or mnemonic.startswith('loop') or mnemonic.startswith('ret'):
        category = 'branches'
    elif spill:
        category = 'spill_stores' if operands and '[' in operands[0] else 'spill_reloads'
    elif mnemonic in ('push', 'pop', 'leave', 'enter') or (mnemonic in ('mov', 'add', 'sub', 'lea') and operands and operands[0] in ('bp', 'sp', 'rbp', 'rsp')):
        category = 'frame_setup'
    elif mnemonic in ('mov', 'movaps', 'movapd', 'movdqa', 'movss', 'movsd') and len(operands) == 2 and all(re.fullmatch(r'[a-z][a-z0-9]*', operand) for operand in operands):
        category = 'register_copies'
    else:
        category = 'everything_else'
    raw = bytes.fromhex(instruction['bytes'])
    prefixes = []
    for byte in raw:
        if byte in (0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0x66, 0x67):
            prefixes.append(byte)
        elif not instruction.get('real') and 0x40 <= byte <= 0x4f:
            continue
        else:
            break
    return category, prefixes.count(0x66), prefixes.count(0x67)


def execute(directory, binary, functions, config, plugin, expected, timeout):
    pcs = sorted({instruction['address'] for function in functions for instruction in function['instructions']})
    (directory / 'pcs.txt').write_text(''.join(f'{pc:x}\n' for pc in pcs))
    plugin_arg = f'{plugin},map={directory}/pcs.txt,out={directory}/counts.txt'
    if config in ('small', 'unreal', 'unreal32'):
        image = build_image(binary.read_bytes(), 0x1000, 0, 'real' if config == 'small' else 'unreal')
        (directory / 'disk.img').write_bytes(image)
        command = ['qemu-system-i386', '-nodefaults', '-display', 'none', '-no-reboot', '-accel', 'tcg', '-smp', '1',
                   '-m', '64', '-drive', f'file={directory}/disk.img,format=raw,if=ide', '-serial', f'file:{directory}/serial.out',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-monitor', 'none', '-plugin', plugin_arg]
        expected_status = (expected << 1) | 1
    else:
        command = ['qemu-x86_64', '-plugin', plugin_arg, binary]
        expected_status = expected
        native = subprocess.run([binary], capture_output=True, timeout=timeout)
        if native.returncode != expected:
            raise RuntimeError(f'native exit {native.returncode}, expected {expected}')
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout)
    (directory / 'qemu.stderr').write_text(result.stderr)
    if result.returncode != expected_status:
        raise RuntimeError(f'QEMU exit {result.returncode}, expected {expected_status}: {result.stderr}')
    counts = {int(pc, 16): int(count) for pc, count in (line.split() for line in (directory / 'counts.txt').read_text().splitlines())}
    for function in functions:
        for instruction in function['instructions']:
            instruction['executions'] = counts[instruction['address']]
    return result.returncode


def measure(entry, config, args, plugin, crt):
    name, source, expected, origin = entry
    directory = args.output / config / name
    directory.mkdir(parents=True, exist_ok=True)
    real = config in ('small', 'unreal', 'unreal32')
    flags = ['-O2', '-fwhole-program', '-target', 'i8086-binary' if real else 'x86_64-elf', '-fbackend', 'llvm' if config == 'llvm' else 'custom']
    if real:
        flags += ['-mcmodel', config]
    commands = []
    def compile(extra):
        command = [args.dcc, *flags, *extra, source]
        commands.append(list(map(str, command)))
        return run(command, timeout=args.compile_timeout)
    try:
        obj = directory / 'program.o'
        compile(['-c', '-o', obj])
        mir = ''
        if config != 'llvm':
            compile(['-fdump-mir', '-o', directory / 'program.mir'])
            mir = (directory / 'program.mir').read_text()
        assembly = directory / 'program.s'
        compile(['-S', '-o', assembly])
        frames = llvm_frames(assembly.read_text()) if config == 'llvm' else mir_frames(mir)
        runtime_frames = llvm_frames((args.output / 'runtime.s').read_text())
        binary = directory / ('program.bin' if real else 'program.elf')
        if real:
            link = [args.dcc, '-target', 'i8086-binary', '-mcmodel', config, '-fbase', '1000:0000', crt['real'], obj,
                    f'-Wl,-Map={directory}/program.map', '-o', binary]
            commands.append(list(map(str, link)))
            run(link, timeout=args.compile_timeout)
            addresses = {}
            for line in (directory / 'program.map').read_text().splitlines():
                match = re.match(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+\s+\d+\s+(\S+)\s*$', line)
                if match:
                    addresses[match[2]] = int(match[1], 16)
            funcs = symbols(obj)
            for function in funcs:
                function['address'] = addresses[function['name']] + 0x10000
            instructions = disassemble(binary, real=True, base=0x10000)
        else:
            run(['ld', '-o', binary, crt['flat'], obj, crt['runtime']])
            funcs = symbols(binary)
            object_names = {function['name'] for function in symbols(obj)}
            undefined = {line.split()[-1] for line in run(['readelf', '-sW', obj]).splitlines() if ' UND ' in line and len(line.split()) >= 8}
            object_names |= undefined & {function['name'] for function in symbols(crt['runtime'])}
            funcs = [function for function in funcs if function['name'] in object_names]
            instructions = disassemble(binary)
        for function in funcs:
            start = function['address']
            function['instructions'] = [instruction.copy() for instruction in instructions if start <= instruction['address'] < start + function['size']]
            if sum(i['size'] for i in function['instructions']) != function['size']:
                raise RuntimeError(f'incomplete disassembly of {function["name"]}')
            frame = frames.get(function['name'], runtime_frames.get(function['name'], {}))
            if config != 'llvm' and function['name'] not in frames and function['name'] not in runtime_frames:
                raise RuntimeError(f'missing frame metadata: {function["name"]}')
            function['frame_size'] = frame.get('frame_size', 0)
            function['spill_slots'] = (len(frame.get('llvm_spill_refs', [])) if config == 'llvm' else sum(slot['spill'] for slot in frame.get('slots', [])))
            function['frame_metadata'] = frame
            function['source_kind'] = 'runtime' if function['name'] in ('strlen', 'memset', 'memcpy', 'memmove', 'memcmp') else 'dc'
            for instruction in function['instructions']:
                instruction['real'] = real
                instruction['category'], instruction['prefix66'], instruction['prefix67'] = classify(instruction, frame)
        execute(directory, binary, funcs, config, plugin, expected, args.run_timeout)
        for function in funcs:
            for scope in ('static', 'executed'):
                counts = Counter({category: 0 for category in CATEGORIES})
                counts.update({'prefix66_bytes': 0, 'prefix67_bytes': 0, 'instructions': 0, 'code_bytes': 0})
                for instruction in function['instructions']:
                    weight = 1 if scope == 'static' else instruction['executions']
                    counts[instruction['category']] += weight
                    counts[instruction['category'] + '_bytes'] += weight * instruction['size']
                    counts['instructions'] += weight
                    counts['code_bytes'] += weight * instruction['size']
                    counts['prefix66_bytes'] += weight * instruction['prefix66']
                    counts['prefix67_bytes'] += weight * instruction['prefix67']
                function[scope] = dict(counts)
        result = {'corpus': name, 'configuration': config, 'origin': origin, 'expected_exit': expected, 'functions': funcs, 'commands': commands}
    except (RuntimeError, subprocess.TimeoutExpired, OSError, KeyError) as error:
        result = {'corpus': name, 'configuration': config, 'error': str(error), 'commands': commands}
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def write_tables(output, results, totals):
    fields = ['configuration', 'corpus', 'function', 'source_kind', 'code_bytes', 'spill_slots', 'frame_size']
    metrics = ['instructions', *CATEGORIES, 'prefix66_bytes', 'prefix67_bytes', *[category + '_bytes' for category in CATEGORIES]]
    fields += [scope + '_' + metric for scope in ('static', 'executed') for metric in metrics]
    with (output / 'functions.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for result in results:
            for function in result.get('functions', []):
                row = dict(configuration=result['configuration'], corpus=result['corpus'], function=function['name'],
                           source_kind=function['source_kind'], code_bytes=function['size'], spill_slots=function['spill_slots'], frame_size=function['frame_size'])
                row.update({scope + '_' + metric: function[scope].get(metric, 0) for scope in ('static', 'executed') for metric in metrics})
                writer.writerow(row)
    lines = ['# Code generation measurements', '', '| Configuration | Programs | Functions | Bytes | Static instructions | Executed instructions | Spill locations | Frame bytes |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for config, total in totals.items():
        lines.append(f"| {config} | {total['programs']} | {total['functions']} | {total['static']['code_bytes']} | {total['static']['instructions']} | {total['executed']['instructions']} | {total['spill_slots']} | {total['frame_size']} |")
    for config, total in totals.items():
        lines += ['', '## ' + config, '', '| Category | Static instructions | Instruction bytes | Executed instructions |', '| --- | ---: | ---: | ---: |']
        for category in CATEGORIES:
            lines.append(f"| {category} | {total['static'][category]} | {total['static'].get(category + '_bytes', 0)} | {total['executed'][category]} |")
        for prefix in ('prefix66_bytes', 'prefix67_bytes'):
            lines.append(f"| {prefix} (overlapping bytes) | {total['static'][prefix]} | — | {total['executed'][prefix]} |")
        functions = [(result['corpus'], function) for result in results if result['configuration'] == config for function in result.get('functions', [])]
        for metric in ('code_bytes', 'instructions'):
            scope = 'static' if metric == 'code_bytes' else 'executed'
            lines += ['', f'### Largest {scope} {metric}', '', '| Program | Function | Bytes | Executed instructions | Spill locations | Frame bytes |', '| --- | --- | ---: | ---: | ---: | ---: |']
            for corpus_name, function in sorted(functions, key=lambda item: item[1][scope][metric], reverse=True)[:10]:
                lines.append(f"| {corpus_name} | `{function['name']}` | {function['size']} | {function['executed']['instructions']} | {function['spill_slots']} | {function['frame_size']} |")
    (output / 'summary.md').write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description='Measure custom i8086/em64t and LLVM -O2 code generation; no optimizer changes.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dcc', type=Path, default=ROOT / 'build/bin/dcc')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', help='substring of corpus name for iteration')
    parser.add_argument('--configs', nargs='+', choices=CONFIGS, default=list(CONFIGS))
    parser.add_argument('--compile-timeout', type=int, default=60)
    parser.add_argument('--run-timeout', type=int, default=30)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    args.dcc = args.dcc.resolve()
    plugin = args.output / 'count.so'
    run(['clang', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', *shlex.split(run(['pkg-config', '--cflags', 'glib-2.0'])), HERE / 'count.c', '-o', plugin])
    crt = {'real': args.output / 'crt16.o', 'flat': args.output / 'crt64.o', 'runtime': args.output / 'runtime.o'}
    run(['clang', '-O2', '-ffreestanding', '-fno-builtin', '-c', HERE / 'runtime.c', '-o', crt['runtime']])
    run(['clang', '-O2', '-ffreestanding', '-fno-builtin', '-S', HERE / 'runtime.c', '-o', args.output / 'runtime.s'])
    run(['nasm', '-f', 'elf32', ROOT / 'tests/i8086/crt0.asm', '-o', crt['real']])
    flat = args.output / 'crt64.asm'
    flat.write_text('bits 64\nsection .text\nglobal _start\nglobal __assert\nextern dcc_main\n_start:\n call dcc_main\n mov edi,eax\n mov eax,60\n syscall\n__assert:\n mov edi,123\n mov eax,60\n syscall\nsection .note.GNU-stack noalloc noexec nowrite progbits\n')
    run(['nasm', '-f', 'elf64', flat, '-o', crt['flat']])
    entries = corpus(args.output / 'sources')
    if args.only:
        entries = [entry for entry in entries if args.only in entry[0]]
    jobs = [(entry, config) for entry in entries for config in args.configs]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(measure, entry, config, args, plugin, crt) for entry, config in jobs]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            print(result['configuration'], result['corpus'], result.get('error', 'PASS'), flush=True)
    results.sort(key=lambda result: (result['configuration'], result['corpus']))
    totals = {}
    for config in args.configs:
        subset = [result for result in results if result['configuration'] == config and 'error' not in result]
        totals[config] = {'programs': len(subset), 'failures': sum(result['configuration'] == config and 'error' in result for result in results)}
        functions = [function for result in subset for function in result['functions']]
        for scope in ('static', 'executed'):
            aggregate = Counter()
            for function in functions:
                aggregate.update(function[scope])
            totals[config][scope] = dict(aggregate)
        totals[config].update({'functions': len(functions), 'spill_slots': sum(f['spill_slots'] for f in functions), 'frame_size': sum(f['frame_size'] for f in functions)})
    report = {'revision': run(['git', '-C', ROOT, 'rev-parse', 'HEAD']).strip(), 'diff': run(['git', '-C', ROOT, 'diff']),
              'dcc_sha256': hashlib.sha256(args.dcc.read_bytes()).hexdigest(), 'qemu': run(['qemu-system-i386', '--version']).strip(),
              'tool_sha256': {str(path.relative_to(HERE)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(HERE.rglob('*')) if path.suffix in ('.py', '.c', '.dc')},
              'totals': totals, 'results': results}
    (args.output / 'measurements.json').write_text(json.dumps(report, indent=2) + '\n')
    write_tables(args.output, results, totals)
    print(json.dumps(totals, indent=2))
    return int(any('error' in result for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
