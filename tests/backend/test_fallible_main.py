from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
checks = 0


def invoke(arguments):
    return subprocess.run(['timeout', '60', str(compiler), *map(str, arguments)], capture_output=True, text=True, timeout=65)


with tempfile.TemporaryDirectory(prefix='dcc-fallible-main-') as temporary:
    root = Path(temporary)
    source, obj = root / 'main.dc', root / 'main.o'
    invalid = [
        ('module main; public u32 main(){return 0;}', 'invalid hosted main return type'),
        ('module main; void main(){}', 'invalid hosted main return type'),
        ('module main; enum E{Bad} Result(i32,E) main(){return Result::Ok(0);}', 'invalid hosted main return type'),
        ('module main; enum E{Bad} enum Status(T){Ok,Err(T)} Status(E) main(){return Status::Ok;}', 'invalid hosted main return type'),
        ('module main; Status(Error) main(i32 value){return Status::Ok;}', 'must have no parameters'),
        ('module main; i32 ordinary(Status(Error) value){return value;} public i32 main(){return 0;}', 'return type mismatch'),
    ]
    for backend in ('llvm', 'custom'):
        for opt in ('-O0', '-O2'):
            for text, diagnostic in invalid:
                source.write_text(text)
                result = invoke(['-flibdcext', 'linux', '-fbackend', backend, opt, '-c', source, '-o', obj])
                assert result.returncode == 1 and diagnostic in result.stderr, result.stderr
                checks += 1
    text = '''module main;
public struct Custom{u64[32] values;}
@nomangle public Status(Custom) main(){Custom value={};value.values[31]=42;return Status::Err(value);}
'''
    source.write_text(text)
    for backend in ('llvm', 'custom'):
        for opt in ('-O0', '-O2'):
            for whole in ([], ['-fwhole-program']):
                result = invoke(['-flibdcext', 'windows', '-target', 'x86_64-coff', '-fbackend', backend, opt, *whole, '-c', source, '-o', obj])
                assert result.returncode == 0, result.stderr
                symbols = subprocess.check_output(['nm', '--defined-only', obj], text=True)
                assert re.search(r'\bT main$', symbols, re.M), symbols
                assert ('__dc_main' in symbols) == bool(whole), symbols
                assert re.search(r'\bT __dcc_status_entry_', symbols), symbols
                checks += 1
            result = invoke(['-flibdcext', 'freestanding', '-target', 'x86_64-elf', '-fbackend', backend, opt, '-c', source, '-o', obj])
            assert result.returncode == 0, result.stderr
            symbols = subprocess.check_output(['nm', '--defined-only', obj], text=True)
            assert '__dc_main' not in symbols and '__dcc_status_entry_' not in symbols, symbols
            assert re.search(r'\bT main$', symbols, re.M), symbols
            checks += 1
    result = invoke(['-flibdcext', 'linux', '-fbackend', 'llvm', '-g', '-c', source, '-o', obj])
    assert result.returncode == 0, result.stderr
    checks += 1
    symbols = subprocess.check_output(['nm', '--defined-only', obj], text=True)
    adapter = next(line.split()[-1] for line in symbols.splitlines() if re.search(r'\bT __dcc_status_entry_', line))
    assert re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', adapter), adapter
    source.write_text(text + f'\n@nomangle public i32 {adapter}(){{return 99;}}\n')
    result = invoke(['-flibdcext', 'linux', '-c', source, '-o', obj])
    assert result.returncode == 1 and 'reserved status entry symbol' in result.stderr, result.stderr
    checks += 1
print(f'  RESULT {checks}/{checks} fallible main diagnostics, Windows ABI and freestanding checks passed')
