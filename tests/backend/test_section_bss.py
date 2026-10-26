from pathlib import Path
import argparse
import os
import re
import struct
import tempfile
from test_memory import run
from library_size import inspect

parser = argparse.ArgumentParser()
parser.add_argument('dcc', type=Path)
parser.add_argument('--windows', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='dcc-section-bss-') as temporary:
    root = Path(temporary)
    source = Path(__file__).with_name('section_bss.dc')
    harness = root / 'harness.c'
    harness.write_text('extern unsigned char zeroes[32768]; extern unsigned char* address(void); int main(void) { if (address()!=zeroes) return 3; for (unsigned i=0; i<32768; ++i) if (zeroes[i]) return 1; zeroes[32767]=42; return address()[32767]==42 ? 0 : 2; }\n')
    for backend in ('llvm', 'custom'):
        for target in ('x86_64-elf', 'x86_64-coff'):
            for opt in ('O0', 'O2'):
                obj, exe = root / 'test.o', root / 'test.exe'
                run([args.dcc, '-target', target, '-fbackend', backend, '-' + opt, '-fdata-sections', '-c', source, '-o', obj])
                assert obj.stat().st_size < 8192, (target, backend, obj.stat().st_size)
                if target.endswith('coff'):
                    data = obj.read_bytes()
                    count, = struct.unpack_from('<H', data, 2)
                    strings = struct.unpack_from('<I', data, 8)[0] + 18 * struct.unpack_from('<I', data, 12)[0]
                    found = False
                    for i in range(count):
                        offset = 20 + 40 * i
                        name = data[offset:offset + 8].split(b'\0')[0]
                        if name.startswith(b'/'):
                            begin = strings + int(name[1:])
                            name = data[begin:data.index(b'\0', begin)]
                        if name.startswith(b'.bss'):
                            raw_size, = struct.unpack_from('<I', data, offset + 16)
                            if not raw_size:
                                continue
                            characteristics, = struct.unpack_from('<I', data, offset + 36)
                            assert characteristics & 0x80 and not characteristics & 0x40, (name, hex(characteristics))
                            assert raw_size == 32768
                            found = True
                    assert found
                    if not args.windows:
                        continue
                    mingw = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw'))
                    cc = mingw / 'bin/x86_64-w64-mingw32-clang'
                else:
                    assert re.search(r'\.bss\S*\s+NOBITS', run(['readelf', '-SW', obj]))
                    cc = 'clang'
                run([cc, harness, obj, '-o', exe])
                assert inspect(exe)['bss'] >= 32768 - 512, (target, backend, inspect(exe))
                run([*([os.environ.get('WINE', 'wine')] if target.endswith('coff') else []), exe])
    print('  RESULT split BSS stays uninitialized without file-backed zero storage; zero initialization and writes execute correctly')
