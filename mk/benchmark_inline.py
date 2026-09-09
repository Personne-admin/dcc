#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from benchmark import ROOT, run, spread, validated_samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'build/benchmarks/inlining.json')
    parser.add_argument('--filter', default='^(array|map|fmt|generic_sort|numeric|question|wrappers|utf8_stream)$')
    parser.add_argument('--cpu', type=int)
    args = parser.parse_args()
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    baseline = json.loads(args.baseline.read_text())
    work = args.output.resolve().parent/(args.output.stem+'-artifacts')
    work.mkdir(parents=True, exist_ok=True)
    runtime = work/'runtime.o'
    rc, _, _, stderr = run(['clang', '-O2', '-ffreestanding', '-fno-builtin', '-c', ROOT/'libdcext/benchmarks/runtime.c', '-o', runtime])
    if rc:
        raise RuntimeError(stderr)
    compiler = ROOT/'build/bin/dcc'
    if hashlib.sha256(compiler.read_bytes()).hexdigest() != baseline['compiler_sha256']:
        raise RuntimeError('Compiler changed since the supplied baseline')
    data = dict(commit=run(['git', 'rev-parse', 'HEAD'])[2].strip(),
                baseline_commit=baseline['commit'], corpus_sha256=baseline['corpus_sha256'],
                opt_version=run(['opt', '--version'])[2], llc_version=run(['llc', '--version'])[2].splitlines()[0],
                affinity=sorted(os.sched_getaffinity(0)), results=[])
    for original in baseline['results']:
        name = original['benchmark']
        if original['config'] != 'llvm:O0' or original['status'] != 'ok' or not re.search(args.filter, name):
            continue
        source = ROOT/'libdcext/benchmarks'/f'{name}.dc'
        directory = work/name
        directory.mkdir(exist_ok=True)
        rc, _, ir, stderr = run([compiler, '-flibdcext', '-O0', '-fdump-llvm', source])
        if rc:
            raise RuntimeError(stderr)
        if 'alwaysinline' in ir:
            raise RuntimeError('Alwaysinline requires an explicit policy for this experiment')
        (directory/'input.ll').write_text(ir)
        sizes = original['functions_before']
        for mode in ['default', 'no-small-inline', 'no-inline']:
            text = ir
            changed = []
            if mode != 'default':
                def disable(match):
                    line = match.group(0)
                    symbol = re.search(r'@([^ (]+)\(', line).group(1)
                    if mode == 'no-small-inline' and not 1 <= sizes.get(symbol, 0) <= 20:
                        return line
                    changed.append(symbol)
                    if ' comdat' in line:
                        return line.replace(' comdat', ' noinline comdat', 1)
                    return line[:-2]+' noinline {'
                text = re.sub(r'^define .* \{$', disable, text, flags=re.M)
            ll = directory/(mode+'.ll')
            optimized = directory/(mode+'-optimized.ll')
            obj = directory/(mode+'.o')
            exe = directory/mode
            ll.write_text(text)
            commands = [
                ['opt', '-S', '-passes=default<O1>', '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic', ll, '-o', optimized],
                ['llc', '-filetype=obj', '-O=2', '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic', optimized, '-o', obj],
                ['ld.lld', '-e', '_start', '-u', '_start', obj, runtime, ROOT/'build/lib/libdcext-linux-llvm.a', '-o', exe],
            ]
            result = dict(benchmark=name, mode=mode, disabled_functions=changed, status='ok')
            for command in commands:
                rc, _, _, stderr = run(command)
                if rc:
                    result.update(status='compile_error', diagnostic=stderr)
                    break
            if result['status'] == 'ok':
                rc, warmup = validated_samples([exe], 1)
                if warmup is None:
                    result.update(status='broken', exit=rc)
                else:
                    rc, samples = validated_samples([exe], 9)
                    if samples is None:
                        result.update(status='broken', exit=rc)
                    else:
                        result.update(runtime_s=spread(samples), binary_bytes=exe.stat().st_size)
                        rc, _, sizes_output, _ = run(['size', exe])
                        if not rc:
                            result['text_bytes'] = int(sizes_output.splitlines()[1].split()[0])
            data['results'].append(result)
            args.output.write_text(json.dumps(data, indent=2)+'\n')
            print(name, mode, result['status'], flush=True)
    return 1 if any(r['status'] != 'ok' for r in data['results']) else 0


if __name__ == '__main__':
    raise SystemExit(main())
