#!/usr/bin/env python3
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime
import tempfile

root = Path(__file__).resolve().parents[2]
checks = [
    ('linux', ['clang'], '#define _GNU_SOURCE\n#include <errno.h>\n#include <fcntl.h>\n#include <sys/mman.h>\n#include <sys/stat.h>\n#include <signal.h>\n#include <time.h>\n#include <sys/random.h>\n',
     {'MAX_ERRNO', 'PAGE_SIZE', 'DEFAULT_FILE_MODE', 'DEFAULT_DIR_MODE', 'STDIN', 'STDOUT', 'STDERR', 'CLOCK_PROCESS_CPUTIME', 'CLOCK_THREAD_CPUTIME'}),
    ('win', [str(Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang')], '#include <windows.h>\n',
     {'PAGE_SIZE', 'ALLOCATION_GRANULARITY', 'FILETIME_UNIX_EPOCH'}),
]

# The platforms declare different sets of constants; assertion totals need not match.
# Each entry covers every declared field: D name, SDK name, offset, byte width.
win_layouts = {
    'Overlapped': ('OVERLAPPED', 32, [
        ('internal', 'Internal', 0, 8), ('internal_high', 'InternalHigh', 8, 8),
        ('offset', 'Pointer', 16, 8), ('event', 'hEvent', 24, 8),
    ]),
    'SecurityAttributes': ('SECURITY_ATTRIBUTES', 24, [
        ('length', 'nLength', 0, 4), ('descriptor', 'lpSecurityDescriptor', 8, 8),
        ('inherit_handle', 'bInheritHandle', 16, 4),
    ]),
    'FileTime': ('FILETIME', 8, [
        ('low', 'dwLowDateTime', 0, 4), ('high', 'dwHighDateTime', 4, 4),
    ]),
    'SystemInfo': ('SYSTEM_INFO', 48, [
        ('oem_id', 'dwOemId', 0, 4), ('page_size', 'dwPageSize', 4, 4),
        ('minimum_application_address', 'lpMinimumApplicationAddress', 8, 8),
        ('maximum_application_address', 'lpMaximumApplicationAddress', 16, 8),
        ('active_processor_mask', 'dwActiveProcessorMask', 24, 8),
        ('number_of_processors', 'dwNumberOfProcessors', 32, 4),
        ('processor_type', 'dwProcessorType', 36, 4),
        ('allocation_granularity', 'dwAllocationGranularity', 40, 4),
        ('processor_level', 'wProcessorLevel', 44, 2),
        ('processor_revision', 'wProcessorRevision', 46, 2),
    ]),
    'ByHandleFileInformation': ('BY_HANDLE_FILE_INFORMATION', 52, [
        ('attributes', 'dwFileAttributes', 0, 4),
        ('creation_time', 'ftCreationTime', 4, 8),
        ('last_access_time', 'ftLastAccessTime', 12, 8),
        ('last_write_time', 'ftLastWriteTime', 20, 8),
        ('volume_serial_number', 'dwVolumeSerialNumber', 28, 4),
        ('file_size_high', 'nFileSizeHigh', 32, 4),
        ('file_size_low', 'nFileSizeLow', 36, 4),
        ('number_of_links', 'nNumberOfLinks', 40, 4),
        ('file_index_high', 'nFileIndexHigh', 44, 4),
        ('file_index_low', 'nFileIndexLow', 48, 4),
    ]),
}


def win_field_assertions(source, directory):
    declarations = dict(re.findall(r'public struct (\w+) \{([^}]+)\}', source))
    assert declarations.keys() == win_layouts.keys(), 'uncovered Win32 structure'
    c_checks = []
    dc_checks = ['module abi_layout; import core; import std::sys::win::abi;',
                 'using std::sys::win::abi; public void check() {']

    def dc_check(expression, label):
        dc_checks.append(f'static if !({expression}) {{ core::compile_error("{label}"); }}')

    for name, (sdk, size, fields) in win_layouts.items():
        declared = dict((field, kind.strip()) for kind, field in
                        re.findall(r'([\w*]+)\s+(\w+);', declarations[name]))
        assert declared.keys() == {field[0] for field in fields}, f'uncovered field in {name}'
        c_checks.append(f'_Static_assert(sizeof({sdk}) == {size}, "{sdk} size");')
        dc_check(f'sizeof(abi::{name}) == {size}', f'{name} size')
        for field, sdk_field, offset, width in fields:
            c_checks.append(f'_Static_assert(__builtin_offsetof({sdk}, {sdk_field}) == {offset}, "{sdk}.{sdk_field} offset");')
            c_checks.append(f'_Static_assert(sizeof((({sdk}*)0)->{sdk_field}) == {width}, "{sdk}.{sdk_field} width");')
            dc_check(f'offsetof(abi::{name}, {field}) == {offset}', f'{name}.{field} offset')
            kind = declared[field]
            dc_type = kind if kind in {'usize', 'u64', 'void*'} else f'abi::{kind}'
            dc_check(f'sizeof({dc_type}) == {width}', f'{name}.{field} width')

    # D's u64 offset represents the SDK's Offset/OffsetHigh union storage.
    c_checks += ['_Static_assert(__builtin_offsetof(OVERLAPPED, Offset) == 16, "Offset");',
                 '_Static_assert(__builtin_offsetof(OVERLAPPED, OffsetHigh) == 20, "OffsetHigh");']
    dc_checks.append('}')
    dc_path = directory / 'layout.dc'
    dc_path.write_text('\n'.join(dc_checks))
    dcc = os.environ.get('DCC', str(root / 'build/bin/dcc'))
    subprocess.run([dcc, '-flibdcext', 'windows', '-target', 'x86_64-coff',
                    '-I' + str(root / 'libdcext'), '-c', '-o', str(directory / 'layout.o'),
                    str(dc_path)], check=True)
    return c_checks

with tempfile.TemporaryDirectory(prefix='dcc-abi-') as tmp:
    for platform, compiler, headers, excluded in checks:
        if len(sys.argv) > 1 and platform not in sys.argv[1:]:
            continue
        source = (root / f'libdcext/std/sys/{platform}/abi.dc').read_text()
        assertions = []
        if platform == 'win':
            declared = set(re.findall(r'public using \w+ (\w+) =', source))
            numeric = set(re.findall(r'public using \w+ (\w+) = -?(?:0x[0-9A-Fa-f]+|[0-9]+);', source))
            assert declared == numeric, 'uncovered non-numeric Win32 constant'
            # PAGE_SIZE and ALLOCATION_GRANULARITY are checked by GetSystemInfo
            # in integration.dc; FILETIME_UNIX_EPOCH is derived below.
        for name, value in re.findall(r'public using \w+ (\w+) = (-?(?:0x[0-9A-Fa-f]+|[0-9]+));', source):
            if name in excluded:
                continue
            assertions.append(f'_Static_assert((unsigned long long)({name}) == (unsigned long long)({value}), "{name}");')

        path = Path(tmp) / f'{platform}.c'
        if platform == 'linux':
            assertions += ['_Static_assert(sizeof(struct stat) == 144, "stat size");', '_Static_assert(__builtin_offsetof(struct stat, st_size) == 48, "stat size offset");']
        else:
            assertions += win_field_assertions(source, Path(tmp))

        path.write_text(headers + '\n'.join(assertions))
        subprocess.run(compiler + ['-Werror', '-c', str(path), '-o', str(Path(tmp) / f'{platform}.o')], check=True)
        if platform == 'win':
            ticks = (datetime(1970, 1, 1) - datetime(1601, 1, 1)).days * 86400 * 10000000
            actual = int(re.search(r'FILETIME_UNIX_EPOCH = (\d+)', source)[1])
            assert actual == ticks
        print(f'{platform}: {len(assertions)} constant/layout assertions match system headers')
