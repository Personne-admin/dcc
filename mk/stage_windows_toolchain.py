"""Stage only the pinned native linker, its runtime DLLs/licenses, and DC import libraries."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

SYSTEM_DLLS = set('advapi32 bcrypt crypt32 dbghelp imagehlp kernel32 kernelbase msvcrt ntdll ole32 oleaut32 psapi rpcrt4 secur32 shell32 shlwapi user32 userenv version winmm ws2_32 wsock32 ucrtbase normaliz comdlg32 gdi32 wintrust'.split())
IMPORT_LIBS = ('kernel32', 'ws2_32', 'advapi32', 'shell32')


def imports(path, readobj):
    result = subprocess.run([readobj, '--file-headers', '--coff-imports', str(path)], capture_output=True, text=True, timeout=30)
    if result.returncode or 'COFF-x86-64' not in result.stdout or 'IMAGE_FILE_EXECUTABLE_IMAGE' not in result.stdout:
        raise ValueError('not a native Windows x64 tool: ' + str(path) + '\n' + result.stderr)
    return [line.split(':', 1)[1].strip() for line in result.stdout.splitlines() if line.strip().startswith('Name:')]


def stage(archive, digest, destination, sysroot, readobj='llvm-readobj'):
    h = hashlib.sha256()
    with archive.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    if h.hexdigest() != digest:
        raise ValueError('native Windows linker archive SHA256 mismatch')
    destination.mkdir(parents=True, exist_ok=True)
    binary = destination / 'bin'
    library = destination / 'lib'
    binary.mkdir(exist_ok=True)
    library.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        members = package.infolist()
        candidates = [PurePosixPath(m.filename) for m in members if PurePosixPath(m.filename).name.lower() == 'ld.lld.exe' and PurePosixPath(m.filename).parent.name.lower() == 'bin']
        if not candidates:
            raise ValueError('native linker missing from archive')
        linker = min(candidates, key=lambda p: len(p.parts))
        entries = {}
        for member in members:
            path = PurePosixPath(member.filename)
            if '\\' in member.filename or ':' in member.filename or '..' in path.parts or path.is_absolute() or (member.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError('unsafe native toolchain archive member')
            if path.parent == linker.parent:
                key = path.name.lower()
                if key in entries:
                    raise ValueError('duplicate native toolchain binary: ' + key)
                entries[key] = member
        pending = ['ld.lld.exe']
        copied = []
        while pending:
            name = pending.pop()
            if name in copied:
                continue
            if name not in entries:
                raise ValueError('linker runtime dependency missing from archive: ' + name)
            member = entries[name]
            target = binary / PurePosixPath(member.filename).name
            if member.file_size > 128 << 20:
                raise ValueError('native linker/runtime file exceeds 128 MiB')
            with package.open(member) as source, target.open('wb') as output:
                shutil.copyfileobj(source, output)
            copied.append(name)
            for dll in imports(target, readobj):
                key = dll.lower()
                if key.removesuffix('.dll') in SYSTEM_DLLS or key.startswith(('api-ms-win-', 'ext-ms-')):
                    continue
                if key not in entries:
                    raise ValueError('unbundled linker dependency: ' + dll)
                pending.append(key)
        licenses = []
        for member in package.infolist():
            path = PurePosixPath(member.filename)
            if member.is_dir() or not any(word in path.name.lower() for word in ('license', 'copying')):
                continue
            try:
                relative_name = path.relative_to(linker.parent.parent)
            except ValueError:
                continue
            if len(relative_name.parts) != 1 and relative_name.parts[:3] != ('x86_64-w64-mingw32', 'share', 'mingw32'):
                continue
            if member.file_size > 256 << 10 or relative_name.suffix.lower() not in ('', '.txt'):
                continue
            relative = Path(*relative_name.parts)
            target = destination / 'share/licenses/llvm-mingw' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(package.read(member))
            licenses.append(str(target.relative_to(destination)))
        if not licenses:
            raise ValueError('native linker archive contains no redistributable license notices')
    for name in IMPORT_LIBS:
        source = sysroot / 'x86_64-w64-mingw32/lib' / ('lib' + name + '.a')
        if not source.is_file():
            raise ValueError('Windows DC import library is missing: ' + str(source))
        shutil.copyfile(source, library / source.name)
    (destination / 'share').mkdir(exist_ok=True)
    (destination / 'share/windows-toolchain.json').write_text(json.dumps({'sha256': digest, 'archive': archive.name,
        'binaries': copied, 'licenses': licenses, 'import_libraries': list(IMPORT_LIBS)}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--readobj', default='llvm-readobj')
    args = parser.parse_args()
    pins = dict(line.split('=', 1) for line in (Path(__file__).parent.parent / 'ci/toolchain/pins.env').read_text().splitlines() if '=' in line)
    version = pins['LLVM_MINGW_RELEASE']
    digest = pins['LLVM_MINGW_WINDOWS_SHA256']
    name = 'llvm-mingw-' + version + '-ucrt-x86_64.zip'
    if args.archive:
        stage(args.archive, digest, args.stage, args.sysroot, args.readobj)
    else:
        with tempfile.TemporaryDirectory(prefix='dcc-native-linker-') as temporary:
            archive = Path(temporary) / name
            request = urllib.request.Request('https://github.com/mstorsjo/llvm-mingw/releases/download/' + version + '/' + name,
                                             headers={'User-Agent': 'dcc-msi-packager'})
            with urllib.request.urlopen(request, timeout=60) as source, archive.open('wb') as output:
                size = 0
                while chunk := source.read(1 << 20):
                    size += len(chunk)
                    if size > 512 << 20:
                        raise ValueError('native linker archive exceeds 512 MiB')
                    output.write(chunk)
            stage(archive, digest, args.stage, args.sysroot, args.readobj)


if __name__ == '__main__':
    main()
