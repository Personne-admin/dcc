from pathlib import Path
import sys
import tempfile
from test_memory import run

with tempfile.TemporaryDirectory(prefix='dcc-link-inputs-') as temporary:
    root = Path(temporary)
    source, provider = root / 'start.dc', root / 'provider.dc'
    source.write_text('module start; @nomangle extern i32 linked_value(); @nomangle public i32 _start() { return linked_value(); }\n')
    provider.write_text('module provider; @nomangle public i32 linked_value() { return 42; }\n')
    for target in ('x86_64-elf', 'x86_64-coff'):
        for backend in ('llvm', 'custom'):
            obj, leaf, archive, exe = root / 'start.obj', root / 'provider.o', root / 'provider.a', root / 'image'
            common = [sys.argv[1], '-target', target, '-fbackend', backend]
            run([*common, '-c', source, '-o', obj])
            run([*common, '-c', provider, '-o', leaf])
            archive.unlink(missing_ok=True)
            run(['ar', 'rcs', archive, leaf])
            run([*common, '-e', '_start', obj, archive, '-o', exe])
            symbols = run(['nm', '--defined-only', exe])
            assert '_start' in symbols and 'linked_value' in symbols, symbols
    print('  RESULT .obj and positional archive inputs select final linking on ELF/COFF and both backends')
