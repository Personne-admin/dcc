#!/usr/bin/env python3
from pathlib import Path
import sys
import tempfile
from test_memory import run

with tempfile.TemporaryDirectory(prefix='dcc-opt-size-') as temporary:
    root = Path(temporary)
    source = Path(__file__).with_name('opt_size.dc')
    harness = root / 'harness.c'
    harness.write_text('extern unsigned size_optimized(unsigned); int main(void) { return size_optimized(41) == 42 ? 0 : 1; }\n')
    for target in ('x86_64-elf', 'x86_64-coff'):
        for opt in ('O2', 'Os'):
            obj = root / (target + opt + '.o')
            output = run([sys.argv[1], '-target', target, '-fbackend', 'llvm', '-' + opt, '-fdump-llvm', source])
            assert ('optsize' in output) == (opt == 'Os'), output
            run([sys.argv[1], '-target', target, '-fbackend', 'llvm', '-' + opt, '-c', source, '-o', obj])
            assert obj.stat().st_size > 0
            if target == 'x86_64-elf':
                exe = root / opt
                run(['clang', '-no-pie', harness, obj, '-o', exe])
                run([exe])
    print('  RESULT LLVM O2/Os attributes, object emission and execution passed')
