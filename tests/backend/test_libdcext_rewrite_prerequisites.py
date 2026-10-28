from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    compiler = args.dcc.resolve()
    root = Path(__file__).resolve().parents[2]
    for fixture_name in ('libdcext-rewrite-prerequisites-exec.dcc-test', 'typeof-exec.dcc-test'):
        fixture = root / 'tests/cases/em64t' / fixture_name
        files = re.findall(r'^=== FILE: ([^\n]+) ===\n(.*?)(?=^=== |\Z)', fixture.read_text(), re.M | re.S)
        assert files and any(name == 'main.dc' for name, body in files)
        with tempfile.TemporaryDirectory(prefix='dcc-rewrite-prerequisites-') as directory:
            scratch = Path(directory)
            for name, body in files:
                (scratch / name).write_text(body)
            harness = scratch / 'harness.c'
            harness.write_text('extern int dcc_main(void); int main(void) { return dcc_main(); }\n')
            target = 'x86_64-coff' if args.windows else 'x86_64-elf'
            cc = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang' if args.windows else 'clang'
            for backend in ('llvm', 'custom'):
                for level in ('-O0', '-O2'):
                    for delivery in ('separate', 'whole'):
                        objects = []
                        sources = ['main.dc'] if delivery == 'whole' else [name for name, body in files]
                        for index, name in enumerate(sources):
                            obj = scratch / ('probe' + str(index) + '.o')
                            run([compiler, '-target', target, '-fbackend', backend, level, '-I', scratch,
                                 *(['-fwhole-program'] if delivery == 'whole' else []), '-c', scratch / name, '-o', obj])
                            objects.append(obj)
                        exe = scratch / 'probe.exe'
                        run([cc, *([] if args.windows else ['-no-pie']), harness, *objects, '-o', exe])
                        run([*([os.environ.get('WINE', 'wine')] if args.windows else []), exe])
                        print('  PASS', fixture_name, target, backend, level, delivery, flush=True)
            for model in ('small', 'unreal', 'unreal32'):
                for level in ('-O0', '-O2'):
                    run([compiler, '-target', 'i8086-binary', '-fbackend', 'custom', '-mcmodel=' + model,
                         level, '-I', scratch, '-c', scratch / 'main.dc', '-o', scratch / 'narrow.o'])
            print('  RESULT 8 executions and 6 narrow compile checks passed', flush=True)


if __name__ == '__main__':
    main()
