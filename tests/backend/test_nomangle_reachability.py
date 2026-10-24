#!/usr/bin/env python3
from pathlib import Path
import sys
import tempfile
from test_memory import run

with tempfile.TemporaryDirectory(prefix='dcc-nomangle-reachability-') as temporary:
    root = Path(temporary)
    include = root / 'include'
    (include / 'std').mkdir(parents=True)
    (include / 'std/prelude.dc').write_text('module std::prelude;\n')
    (include / 'assert.dc').write_text('module assert; @nomangle @noreturn public void __assert() { while true {} }\n')
    (include / 'provider.dc').write_text('module provider; @nomangle public i32 linked_value() { return 42; }\n')
    (include / 'libdcext.manifest').write_text('dcc-libdcext-manifest 1\nsource all provider provider.dc\nsource all assert assert.dc\n')
    source = root / 'main.dc'
    source.write_text('''module main;
import provider;
@nomangle extern i32 linked_value();
@nomangle public i32 probe() { return if linked_value() == 42 { 0 } else { 1 }; }
asm { ".text\\n.globl _start\\n_start:\\nand $-16, %%rsp\\ncall probe\\nmov %%eax, %%edi\\nmov $60, %%eax\\nsyscall\\nud2\\n.section .note.GNU-stack,\\"\\",@progbits\\n" };
''')
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            obj, exe = root / 'app.o', root / 'app'
            run([sys.argv[1], '-I', include, '-flibdcext', 'freestanding', '-fwhole-program', '-e', '_start',
                 '-target', 'x86_64-elf', '-fbackend', backend, '-' + opt, '-c', source, '-o', obj])
            assert ' T linked_value' in run(['nm', '--defined-only', obj])
            run(['ld.lld', '--static', '--fatal-warnings', '-e', '_start', obj, '-o', exe])
            run([exe])
    print('  RESULT whole-program extern aliases retain matching library definitions')
