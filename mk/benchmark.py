#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import time
from benchmark_corpus import generate

ROOT = Path(__file__).resolve().parents[1]


def run(command, timeout=180, env=None):
    start = time.perf_counter()
    try:
        p = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=timeout, env=env)
        return p.returncode, time.perf_counter() - start, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, None, '', 'timeout'


def validated_samples(command, count, env=None, timeout=15):
    samples = []
    for _ in range(count):
        rc, seconds, stdout, stderr = run(command, timeout=timeout, env=env)
        if rc or stdout:
            return rc, None
        samples.append(seconds)
    return 0, samples


def spread(values):
    return dict(median=statistics.median(values), min=min(values), max=max(values),
                mad=statistics.median(abs(x-statistics.median(values)) for x in values), samples=values)


def metrics(stderr):
    phases = {}
    result = {}
    for line in stderr.splitlines():
        fields = line.split()
        if fields[:2] == ['DCC_BENCH', 'phase']:
            phases[fields[2]] = phases.get(fields[2], 0) + float(fields[3])
        elif fields[:2] == ['DCC_BENCH', 'static']:
            stage = fields[2]
            stats = dict(zip(['instructions', 'calls', 'small_calls', 'naive_inline_added_ir'], map(int, fields[3:])))
            result['static_'+stage] = stats
            result['ir_'+stage] = stats['instructions']
            if stage == 'before':
                result.update({k: v for k, v in stats.items() if k != 'instructions'})
        elif fields[:2] == ['DCC_BENCH', 'memory']:
            result['memory_'+fields[2]] = dict(zip(['loads', 'stores', 'allocas', 'conditional_branches', 'phis'], map(int, fields[3:])))
        elif fields[:2] == ['DCC_BENCH', 'function']:
            result.setdefault('functions_'+fields[2], {})[fields[4]] = int(fields[3])
        elif fields[:2] == ['DCC_BENCH', 'ir']:
            result['ir_after'] = int(fields[3])
        elif fields[:2] == ['DCC_BENCH', 'partial']:
            for key, val in zip(['partial_attempts', 'partial_succeeded', 'partial_fallbacks'], map(int, fields[2:5])):
                result[key] = result.get(key, 0) + val
        elif fields[:2] == ['DCC_BENCH', 'partial_reason']:
            bucket = result.setdefault('partial_reasons', {})
            reason = ' '.join(fields[3:])
            bucket[reason] = bucket.get(reason, 0) + int(fields[2])
        elif fields[:2] == ['DCC_BENCH', 'partial_callee']:
            bucket = result.setdefault('partial_callees', {})
            name, _, path = ' '.join(fields[3:]).partition(' @ ')
            key = name + ' @ ' + path
            bucket[key] = bucket.get(key, 0) + int(fields[2])
        elif fields[:2] == ['DCC_BENCH', 'partial_attempt']:
            bucket = result.setdefault('partial_attempted', {})
            name, _, path = ' '.join(fields[3:]).partition(' @ ')
            key = name + ' @ ' + path
            bucket[key] = bucket.get(key, 0) + int(fields[2])

    if 'ir_before' in result:
        result.setdefault('ir_after', result['ir_before'])
        for key in ['static', 'memory', 'functions']:
            if key+'_before' in result:
                result.setdefault(key+'_after', result[key+'_before'])
    if 'frontend' in phases:
        phases['sema_imports_other'] = phases['frontend'] - phases.get('parse', 0)

    result['phases_s'] = phases
    return result


def profile(exe, directory, valgrind, env):
    output = directory / 'callgrind.out'
    command = [valgrind, '--tool=callgrind', '--cache-sim=yes', '--branch-sim=yes',
               '--callgrind-out-file='+str(output), exe]

    rc, _, stdout, stderr = run(command, timeout=180, env=env)
    (directory / 'profile.log').write_text(stdout+stderr)
    if rc or stdout:
        return {'status': 'unavailable', 'exit': rc, 'diagnostic': stderr[-1500:]}

    data = output.read_text()
    events = re.search(r'^events: (.*)$', data, re.M).group(1).split()
    totals = re.search(r'^summary: (.*)$', data, re.M).group(1).split()
    result = dict(status='ok', events=dict(zip(events, map(int, totals))))
    annotate = Path(valgrind).with_name('callgrind_annotate')
    rc, _, stdout, stderr = run([annotate, '--auto=no', '--threshold=95', output])
    if not rc:
        (directory / 'profile.txt').write_text(stdout)
        result['annotation'] = os.path.relpath(directory / 'profile.txt', ROOT)

    return result


def build_library(compiler, backend, opt, directory):
    directory.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in sorted((ROOT/'libdcext/std').rglob('*.dc')):
        obj = directory/(str(source.relative_to(ROOT/'libdcext')).replace('/', '_')+'.o')
        command = [compiler, '-c', '-flibdcext', 'linux', '-target', 'x86_64-elf', '-fbackend='+backend,
                   '-'+opt, '-I'+str(ROOT/'libdcext'), '-o', obj, source]
        rc, _, stdout, stderr = run(command)
        if rc:
            raise RuntimeError(str(source)+'\n'+stdout+stderr)
        objects.append(obj)
    for source in sorted((ROOT/'libdcext/linux').glob('*.asm')):
        obj = directory/(source.name+'.o')
        rc, _, _, stderr = run(['clang', '-target', 'x86_64-elf', '-c', source, '-o', obj])
        if rc:
            raise RuntimeError(stderr)
        objects.append(obj)
    archive = directory/'libdcext.a'
    rc, _, _, stderr = run(['llvm-ar', 'rcs', archive, *objects])
    if rc:
        raise RuntimeError(stderr)
    return archive


def summary(data, baseline=None):
    lines = ['benchmark | config | status | compile ms | runtime ms (MAD) | bytes | IR before/after',
             '--- | --- | --- | ---: | ---: | ---: | ---:']

    if baseline and data.get('corpus_sha256') != baseline.get('corpus_sha256'):
        lines.insert(0, 'WARNING: corpus hashes differ; ratios are not controlled comparisons.\n')
    old = {(r['benchmark'], r['config']): r for r in (baseline or {}).get('results', [])}
    for r in data['results']:
        t = r.get('runtime_s')
        runtime = f"{1000*t['median']:.3f} ({1000*t['mad']:.3f})" if t else '—'
        compile_time = r.get('compile_s', {}).get('median')
        compile_text = f'{compile_time*1000:.2f}' if compile_time is not None else '—'
        lines.append(f"{r['benchmark']} | {r['config']} | {r['status']} | {compile_text} | {runtime} | {r.get('binary_bytes', '—')} | {r.get('ir_before', '—')}/{r.get('ir_after', '—')}")
        prior = old.get((r['benchmark'], r['config']), {})
        if t and prior.get('runtime_s'):
            lines.append(f"baseline runtime ratio (current/previous): {t['median']/prior['runtime_s']['median']:.3f}")

    return '\n'.join(lines)+'\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT/'build/benchmarks/baseline.json')
    p.add_argument('--baseline', type=Path)
    p.add_argument('--configs', default='llvm:O0,llvm:O1,llvm:O2,llvm:Os,em64t:O0,em64t:O1,em64t:O2,em64t:Os')
    p.add_argument('--filter', default='.*')
    p.add_argument('--compile-runs', type=int, default=3)
    p.add_argument('--runs', type=int, default=9)
    p.add_argument('--profile', action='store_true')
    p.add_argument('--profile-existing', action='store_true', help='profile saved successful binaries without repeating timings')
    p.add_argument('--valgrind', default=shutil.which('valgrind'))
    p.add_argument('--cpu', type=int)
    p.add_argument('--compiler-build', default=os.environ.get('BUILD_TYPE', 'unknown'))
    p.add_argument('--library-opt', choices=['O0', 'O1', 'O2', 'Os'], help='build private archives at this level; default uses shipped O2 archives')
    p.add_argument('--allow-broken', action='store_true')
    p.add_argument('--extra-compile-flags', default='', help='extra flags appended to each dcc compile command')
    p.add_argument('--perf', help='path to perf for hardware counters')
    args = p.parse_args()

    if args.runs < 3 or args.compile_runs < 1:
        p.error('at least three runtime samples and one compile sample are required')

    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})

    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.profile_existing:
        if not args.valgrind:
            p.error('--profile-existing requires --valgrind')
        data = json.loads(args.output.read_text())
        for r in data['results']:
            if r['status'] != 'ok' or not re.search(args.filter, r['benchmark']) or r['config'] not in args.configs.split(','):
                continue
            directory = args.output.parent/(args.output.stem+'-artifacts')/r['benchmark']/r['config'].replace(':', '-')
            r['profile'] = profile(directory/'program', directory, args.valgrind, dict(os.environ))
            args.output.write_text(json.dumps(data, indent=2)+'\n')
            print(r['benchmark'], r['config'], r['profile']['status'], flush=True)
        return 0
    work = args.output.parent / (args.output.stem+'-artifacts')
    work.mkdir(exist_ok=True)
    compiler = ROOT/'build/bin/dcc'
    runtime = work/'runtime.o'
    rc, _, _, err = run(['clang', '-O2', '-ffreestanding', '-fno-builtin', '-c', ROOT/'libdcext/benchmarks/runtime.c', '-o', runtime])
    if rc:
        raise RuntimeError(err)

    libraries = {b: ROOT/f'build/lib/libdcext-linux-{b}.a' for b in ['llvm', 'em64t']}
    if args.library_opt:
        for backend in sorted({c.split(':')[0] for c in args.configs.split(',')}):
            libraries[backend] = build_library(compiler, backend, args.library_opt, work/('library-'+backend+'-'+args.library_opt))
    sources = sorted((ROOT/'libdcext/benchmarks').glob('*.dc'))
    sources += generate(work/'corpus')
    corpus = hashlib.sha256((ROOT/'mk/benchmark_corpus.py').read_bytes())
    for path in sorted((ROOT/'libdcext/benchmarks').rglob('*')):
        if path.is_file() and path.suffix in ['.dc', '.c']:
            corpus.update(str(path.relative_to(ROOT)).encode() + path.read_bytes())

    data = dict(schema=1, commit=run(['git', 'rev-parse', 'HEAD'])[2].strip(),
                dirty=bool(run(['git', 'status', '--porcelain'])[2]),
                library_sha256={b: hashlib.sha256(libraries[b].read_bytes()).hexdigest() for b in ['llvm', 'em64t']},
                compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                corpus_sha256=corpus.hexdigest(), date=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                host=platform.platform(), cpu=Path('/proc/cpuinfo').read_text().split('model name')[1].split('\n')[0].strip(': \t'),
                affinity=sorted(os.sched_getaffinity(0)),
                tools={name: run([name, '--version'])[2].splitlines()[0] for name in ['clang', 'ld.lld', 'llvm-ar', 'objdump']}, compiler_build=args.compiler_build,
                options=vars(args).copy(), results=[])

    data['options'] = {k: str(v) if isinstance(v, Path) else v for k, v in data['options'].items()}
    env = dict(os.environ, DCC_BENCH_STATS='1')
    clean_env = dict(os.environ)
    clean_env.pop('DCC_BENCH_STATS', None)
    baseline = json.loads(args.baseline.read_text()) if args.baseline else None
    for source in sources:
        name = source.parent.name if source.name == 'main.dc' else source.stem
        if not re.search(args.filter, name):
            continue

        for config in args.configs.split(','):
            fields = config.split(':')
            backend, opt = fields[:2]
            for e in (env, clean_env):
                e.pop('DCC_BENCH_SKIP_IR_PASSES', None)
                if fields[2:] == ['no-ir']:
                    e['DCC_BENCH_SKIP_IR_PASSES'] = '1'

            directory = work/name/config.replace(':', '-')
            directory.mkdir(parents=True, exist_ok=True)
            obj, exe = directory/'program.o', directory/'program'
            command = [compiler, '-flibdcext', '-target', 'x86_64-elf', '-fbackend='+backend, '-'+opt, *args.extra_compile_flags.split(), '-c', '-o', obj, source]
            link = ['ld.lld', '-e', '_start', '-u', '_start', obj, runtime, libraries[backend], '-o', exe]
            r = dict(benchmark=name, config=config, status='compile_error', command=list(map(str, command)))
            rc, _, stdout, stderr = run(command, env=env)
            (directory/'compile.log').write_text(stdout+stderr)
            r.update(metrics(stderr))
            if not rc:
                r['status'] = 'link_error'
                rc, _, stdout, stderr = run(link)
                (directory/'link.log').write_text(stdout+stderr)

            if not rc:
                rc, _, stdout, stderr = run([exe], timeout=15, env=clean_env)
                r.update(status='broken' if rc or stdout else 'ok', validation_exit=rc)
                (directory/'validation.log').write_text(stdout+stderr)

            if r['status'] == 'ok':
                compile_times, link_times = [], []
                for _ in range(args.compile_runs):
                    rc, seconds, _, stderr = run(command, env=clean_env)
                    if rc:
                        r['status'] = 'compile_error'
                        break

                    rc, link_seconds, _, stderr = run(link)
                    if rc:
                        r['status'] = 'link_error'
                        break

                    rc, _, stdout, stderr = run([exe], timeout=15, env=clean_env)
                    if rc or stdout:
                        r['status'] = 'broken'
                        break

                    compile_times.append(seconds+link_seconds)
                    link_times.append(link_seconds)

                if r['status'] == 'ok':
                    rc, samples = validated_samples([exe], args.runs, clean_env)
                    if samples is None:
                        r.update(status='broken', validation_exit=rc)
                if r['status'] == 'ok':
                    r.update(compile_s=spread(compile_times), link_s=spread(link_times), runtime_s=spread(samples),
                             binary_bytes=exe.stat().st_size, object_bytes=obj.stat().st_size)

                    rc, _, asm, _ = run(['objdump', '-d', '--no-show-raw-insn', exe])
                    if not rc:
                        r['machine_instructions'] = len(re.findall(r'^\s*[0-9a-f]+:\s+\S', asm, re.M))
                        r['machine_calls'] = len(re.findall(r'\bcall[q]?\s', asm))

                    rc, _, sizes, _ = run(['size', exe])
                    if not rc:
                        r['text_bytes'] = int(sizes.splitlines()[1].split()[0])

                    if args.perf:
                        counters = directory/'perf.csv'
                        rc, _, stdout, stderr = run([args.perf, 'stat', '-x,', '-o', counters, '-e',
                            'cycles,instructions,branches,branch-misses,cache-references,cache-misses', exe], timeout=30, env=clean_env)
                        r['perf'] = {'status': 'ok' if rc == 0 and not stdout else 'unavailable', 'raw': counters.read_text() if counters.exists() else stderr}

                    if args.profile and args.valgrind:
                        r['profile'] = profile(exe, directory, args.valgrind, clean_env)

            r['exit'] = rc
            data['results'].append(r)
            args.output.write_text(json.dumps(data, indent=2)+'\n')
            args.output.with_suffix('.md').write_text(summary(data, baseline))
            print(name, config, r['status'], flush=True)

    print('Results:', args.output)
    return 1 if any(r['status'] != 'ok' and not (args.allow_broken and r['status'] == 'broken') for r in data['results']) else 0


if __name__ == '__main__':
    raise SystemExit(main())
