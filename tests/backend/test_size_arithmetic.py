#!/usr/bin/env python3
from pathlib import Path
import argparse
import tempfile
from test_memory import run
from size_execution import ROOT, native, portable


def panic_checks(dcc, scratch, windows):
    cases = [
        ('std::mem::align_up((~(0 as usize)), 2)', 'mem::align_up: size overflow'),
        ('std::mem::align_up(1, 0)', 'mem::checked_align_up: alignment must be a nonzero power of two'),
        ('std::mem::align_up(1, 3)', 'mem::checked_align_up: alignment must be a nonzero power of two'),
        ('std::math::next_power_of_two((1 as u64 << 63) + 1)', 'math::next_power_of_two: size overflow'),
    ]
    for expression, message in cases:
        source = scratch / 'panic.dc'
        source.write_text('''module main;
import std::mem;
import std::math;
import std::debug;
import std::os::process;
import core::source_location;
void handler([]const char message, core::source_location::SourceLocation sl) {
    if std::mem::equal(message, "MESSAGE") && sl.line != 0 { std::os::process::exit(0); }
    std::os::process::exit(1);
}
public i32 main() {
    std::debug::set_panic_handler(handler);
    EXPRESSION;
    return 90;
}
'''.replace('MESSAGE', message).replace('EXPRESSION', expression))
        native(dcc, scratch, windows, source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    with tempfile.TemporaryDirectory(prefix='dcc-size-arithmetic-') as temp:
        scratch = Path(temp)
        for name in ('size-arithmetic.dc', 'size-format.dc', 'map-failure.dc', 'sort-boundary.dc'):
            source = ROOT / 'tests/stdlib' / name
            native(dcc, scratch, args.windows, source)
            if not args.windows:
                portable(dcc, scratch, source)
        if not args.windows:
            imports = scratch / 'integer-imports.dc'
            modules = ('mem', 'array', 'map', 'sort', 'fmt', 'math', 'hash', 'sha', 'json', 'toml')
            imports.write_text('module imports;\n' + ''.join('import std::' + m + ';\n' for m in modules)
                               + '@nomangle public i32 probe() { return 0; }\n')
            for target in ('x86-elf', 'x86-coff', 'x86_64-elf', 'x86_64-coff', 'i8086-binary'):
                backends = ('llvm', 'custom') if target.startswith('x86_64') else (('custom',) if target == 'i8086-binary' else ('llvm',))
                for backend in backends:
                    for model in (('small', 'unreal', 'unreal32') if target == 'i8086-binary' else ('',)):
                        run([dcc, '-target', target, '-fbackend', backend, '-fno-simd', '-fno-x87',
                             *(['-mcmodel=' + model] if model else []), '-I', ROOT / 'libdcext',
                             '-fwhole-program', '-e', 'probe', '-c', imports, '-o', scratch / 'imports.o'])
        native(dcc, scratch, args.windows, ROOT / 'tests/stdlib/size-os.dc')
        panic_checks(dcc, scratch, args.windows)
    print('  RESULT checked size arithmetic, unchanged owners, narrow imports and panic contracts passed')


if __name__ == '__main__':
    main()
