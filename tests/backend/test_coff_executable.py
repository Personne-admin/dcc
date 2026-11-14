from pathlib import Path
import argparse
import os
import subprocess
import tempfile


def run(argv, **kwargs):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=60, **kwargs)
    assert result.returncode == 0, (argv, result.stdout, result.stderr)
    return result.stdout


def inspect(output):
    headers = run(['llvm-readobj', '--file-headers', '--coff-imports', output])
    assert 'COFF-x86-64' in headers and 'IMAGE_FILE_EXECUTABLE_IMAGE' in headers, headers
    assert 'IMAGE_SUBSYSTEM_WINDOWS_CUI' in headers and 'ImageBase: 0x140000000' in headers, headers
    assert 'KERNEL32.DLL' in headers.upper(), headers
    for name in ('ExitProcess', 'WriteFile', 'VirtualAlloc'):
        assert name in headers, (name, headers)
    imported = [line.split(':', 1)[1].strip().lower() for line in headers.splitlines() if line.strip().startswith('Name:')]
    assert all(name.endswith('.dll') for name in imported), imported
    assert output.read_bytes()[:2] == b'MZ'


def main():
    parser = argparse.ArgumentParser(description='Cross-link real hosted PE programs; execution requires the native Windows smoke gate.')
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--import-libs', type=Path, required=True)
    args = parser.parse_args()
    dcc, libraries = args.dcc.resolve(), args.import_libs.resolve()
    for name in ('kernel32', 'ws2_32', 'advapi32', 'shell32'):
        assert (libraries / ('lib' + name + '.a')).is_file(), name
    checks = 0
    with tempfile.TemporaryDirectory(prefix='dcc PE \u5de5\u5177 ') as temporary:
        root = Path(temporary)
        source = root / 'hello world.dc'
        source.write_text('module main; public Status(Error) main() { println("Hello, Windows!")?; '
                          'Array(i32) values = {}; defer values.deinit(); values.push(42)?; '
                          'println("Value: {}", values.items()[0])?; return Status::Ok; }\n')
        env = dict(os.environ, TMPDIR=str(root))
        for backend in ('llvm', 'custom'):
            for level in ('0', '2'):
                for mode in ('-fwhole-program', '-fno-whole-program'):
                    output = root / ('hello-' + backend + level + mode + '.exe')
                    run([dcc, source, '-o', output, '-target', 'x86_64-coff', '-flibdcext', 'windows',
                         '-fbackend', backend, '-O' + level, mode, '-L', libraries], env=env)
                    inspect(output)
                    checks += 1
                obj = root / ('hello-' + backend + level + '.obj')
                run([dcc, '-c', source, '-o', obj, '-target', 'x86_64-coff', '-flibdcext', 'windows', '-fbackend', backend, '-O' + level], env=env)
                output = root / ('linked-' + backend + level + '.exe')
                run([dcc, obj, '-o', output, '-target', 'x86_64-coff', '-flibdcext', 'windows', '-fbackend', backend, '-L', libraries], env=env)
                inspect(output)
                checks += 1
        missing = subprocess.run([str(dcc), str(source), '-target', 'x86_64-coff', '-flibdcext', 'windows', '-o', str(root / 'missing.exe')],
                                 capture_output=True, text=True, timeout=60, env=env)
        assert missing.returncode != 0 and "Windows import library 'libkernel32.a' not found" in missing.stderr, missing.stderr
        assert not (root / 'missing.exe').exists()
        checks += 1
        broken = root / 'broken.dc'
        broken.write_text('module broken; public i32 main() { return unknown; }\n')
        result = subprocess.run([str(dcc), str(broken), '-target', 'x86_64-coff', '-o', str(root / 'broken.exe')],
                                capture_output=True, text=True, timeout=60)
        assert result.returncode != 0 and 'unknown' in result.stderr and not (root / 'broken.exe').exists()
        checks += 1
    print(f'PASS {checks} hosted PE cross-link and diagnostics checks (native execution not performed)')


if __name__ == '__main__':
    main()
