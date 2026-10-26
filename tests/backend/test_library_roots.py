#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]


def failed(command, *messages):
    result = subprocess.run(['timeout', '60', *map(str, command)], capture_output=True, text=True, timeout=65)
    assert result.returncode == 1, (command, result.returncode, result.stderr)
    for message in messages:
        assert message in result.stderr, (message, result.stderr)


def execute(exe, windows):
    run([os.environ.get('WINE', 'wine'), exe] if windows else [exe])


def linker(windows):
    return Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang' if windows else 'clang'


def native_mains(dcc, scratch, windows):
    target, variant = ('x86_64-coff', 'windows') if windows else ('x86_64-elf', 'linux')
    source, harness = scratch / 'main.dc', scratch / 'main.c'
    harness.write_text('#include <stddef.h>\nextern int __dc_main(size_t, const char**);\nint main(void) { const char* args[] = {"one", "two"}; return __dc_main(2, args) == 42 ? 0 : 1; }\n')
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            for whole in (False, True):
                flags = [dcc, '-flibdcext', variant, '-target', target, '-fbackend', backend, '-' + opt,
                         *(['-fwhole-program', '-e', 'main'] if whole else [])]
                archives = [] if whole else [dcc.parent.parent / 'lib' / ('libdcext-' + variant + '-' + backend + '.a')]
                for arguments in (False, True):
                    signature = '[] const char* args' if arguments else ''
                    expression = ('args.len as i32 + ' + ('42' if windows else '40')) if arguments else '42'
                    source.write_text(f'module main;\n@nomangle extern i32 __dc_main(usize argc, const char** argv);\npublic i32 main({signature}) {{ return {expression}; }}\n')
                    obj, exe = scratch / 'main.o', scratch / 'main.exe'
                    run([*flags, '-c', source, '-o', obj])
                    assert ('__dc_main' in run(['nm', '--defined-only', obj])) == whole
                    if whole:
                        run([linker(windows), *([] if windows else ['-no-pie']), harness, obj, *archives,
                             *(['-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32'] if windows else []), '-o', exe])
                        execute(exe, windows)
                    else:
                        system = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'x86_64-w64-mingw32/lib'
                        run([dcc, '-flibdcext', variant, '-target', target, '-fbackend', backend, obj,
                             *(['-L', system, '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32'] if windows else []), '-o', exe])
                        result = subprocess.run(['timeout', '60', *([os.environ.get('WINE', 'wine')] if windows else []), str(exe), 'one'], capture_output=True, timeout=65)
                        assert result.returncode == 42, (result.returncode, result.stderr)
                source.write_text('module main;\npublic i32 main() { return 0; }\npublic i32 main([] const char* args) { return args.len as i32; }\n')
                failed([*flags, '-c', source, '-o', obj], 'ambiguous program main', 'i32 main()', 'i32 main([] const char*)', 'main.dc:2', 'main.dc:3')
                print('  PASS main thunk', variant, backend, opt, 'whole' if whole else 'separate', flush=True)


def graph(dcc, scratch, windows):
    original_dcc = dcc
    (scratch / 'bin').mkdir()
    dcc = scratch / 'bin/dcc'
    try:
        os.link(original_dcc, dcc)
    except OSError:
        shutil.copy2(original_dcc, dcc)
    (scratch / 'lib').symlink_to(original_dcc.parent.parent / 'lib', target_is_directory=True)
    include = scratch / 'include'
    include.mkdir()
    (include / 'std').mkdir()
    for child in (original_dcc.parent.parent / 'include/std').iterdir():
        if child.name != 'prelude.dc':
            (include / 'std' / child.name).symlink_to(child, target_is_directory=child.is_dir())
    (include / 'std/prelude.dc').write_text('module std::prelude;\n')
    (include / 'common').symlink_to(original_dcc.parent.parent / 'include/common', target_is_directory=True)
    (include / 'assert.dc').symlink_to(original_dcc.parent.parent / 'include/assert.dc')
    manifest = (ROOT / 'libdcext/libdcext.manifest').read_text()
    manifest += 'source all leaves leaves.dc\nsource all dead_leaves dead_leaves.dc\nsource all registry registry.dc\nrecord all registry registration .dcc.registry\n'
    (include / 'libdcext.manifest').write_text(manifest)
    section = 'xr' if windows else 'ax'
    (include / 'leaves.dc').write_text('''module leaves;
@nomangle public extern i32 live_leaf();
@nomangle public i32 raw_callback() { return 14; }
@nomangle public i32 table_callback() { return 28; }
@nomangle public i32 dead_callback() { return 99; }
public using Callback = i32(*)();
public struct Table { Callback call; }
@nomangle public Table live_table = {call = table_callback};
@nomangle public Table dead_table = {call = dead_callback};
''' + f'asm {{ ".section .text.live, \\"{section}\\"\\n.globl live_leaf\\nlive_leaf:\\nsub $40, %%rsp\\ncall raw_callback\\nadd $40, %%rsp\\nret\\n" }};\n')
    (include / 'dead_leaves.dc').write_text('module dead_leaves;\n@nomangle public extern i32 dead_leaf();\n@nomangle public i32 default_provider() { return 99; }\n' +
        f'asm {{ ".section .text.dead, \\"{section}\\"\\n.globl dead_leaf\\ndead_leaf:\\ncall %{{default_provider}}\\ncall unavailable_provider\\nret\\n" }};\n')
    (include / 'registry.dc').write_text('''module registry;
public using Callback = i32(*)();
@nomangle public i32 record_callback() { return 9; }
@nomangle public Callback registration = record_callback;
''')
    (include / 'app_exports.dc').write_text('module app_exports;\n@nomangle public i32 imported_application_export() { return 5; }\n')
    entry, harness = scratch / 'graph.dc', scratch / 'graph.c'
    entry.write_text('''module graph;
import leaves;
import dead_leaves;
import registry;
import app_exports;
@nomangle public i32 application_export() { return 7; }
@nomangle public i32 run_probe() { return leaves::live_leaf() + leaves::live_table.call(); }
''')
    harness.write_text(('const unsigned char __dcc_libdcext_abi_requires_float_disabled = 0;\n' if windows else '') +
                       'extern int run_probe(void);\nint main(void) { return run_probe() == 42 ? 0 : 1; }\n')
    target = 'x86_64-coff' if windows else 'x86_64-elf'
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            for whole in (False, True):
                flags = [dcc, '-flibdcext', 'freestanding', '-target', target, '-fbackend', backend, '-' + opt,
                         '-fno-simd', '-fno-x87', '-ffunction-sections', '-fdata-sections', '-I', include]
                obj, record, exe = scratch / 'graph.o', scratch / 'record.o', scratch / 'graph.exe'
                if whole:
                    run([*flags, '-fwhole-program', '-e', 'run_probe', '--print-reachability', '-c', entry, '-o', obj])
                    symbols = run(['nm', obj])
                    for symbol in ('application_export', 'imported_application_export', 'live_leaf', 'raw_callback', 'table_callback', 'registration', 'record_callback'):
                        assert symbol in symbols, (symbol, symbols)
                    for symbol in ('dead_leaf', 'dead_table', 'dead_callback', 'default_provider', 'unavailable_provider'):
                        assert symbol not in symbols, (symbol, symbols)
                    inputs = [obj]
                else:
                    run([*flags, '-c', entry, '-o', obj])
                    leaf = scratch / 'leaves.o'
                    run([*flags, '-c', include / 'leaves.dc', '-o', leaf])
                    run([*flags, '-c', include / 'registry.dc', '-o', record])
                    archive = scratch / 'libregistry.a'
                    archive.unlink(missing_ok=True)
                    run(['ar', 'rcs', archive, record])
                    unused_object = scratch / 'unused.o'
                    run([*flags, '-c', include / 'dead_leaves.dc', '-o', unused_object])
                    unused_archive = scratch / 'libunused.a'
                    unused_archive.unlink(missing_ok=True)
                    run(['ar', 'rcs', unused_archive, unused_object])
                    inputs = [obj, leaf, unused_archive, archive]
                library_name = ('libdcext-windows-' if windows else 'libdcext-freestanding-') + backend + '.a'
                libs = [] if whole else [dcc.parent.parent / 'lib' / library_name]
                harness.write_text(('const unsigned char __dcc_libdcext_abi_requires_float_disabled = 0;\n' if windows and not whole else '') + 'extern int run_probe(void);\nint main(void) { return run_probe() == 42 ? 0 : 1; }\n')
                run([linker(windows), *([] if windows else ['-no-pie']), '-Wl,--gc-sections', '-Wl,--undefined=registration', harness, *inputs, *libs, '-o', exe])
                execute(exe, windows)
                symbols = run(['nm', exe])
                assert 'registration' in symbols and 'record_callback' in symbols, symbols
                assert 'dead_leaf' not in symbols and 'unavailable_provider' not in symbols, symbols
                if windows:
                    sections = run(['objdump', '-h', exe])
                    assert '.dcc.registry' in run(['objdump', '-h', obj if whole else record])
                    assert '.dcc ' in sections, sections
                else:
                    assert '.dcc.registry' in run(['readelf', '-SW', exe])
                if not windows:
                    startup = scratch / 'startup.dc'
                    startup.write_text('module startup;\nasm { ".text\\n.globl _start\\n_start:\\nand $-16, %%rsp\\ncall run_probe\\nsub $42, %%eax\\nmov %%eax, %%edi\\nmov $60, %%eax\\nsyscall\\n" };\n')
                    startup_object = scratch / 'startup.o'
                    run([*flags, '-c', startup, '-o', startup_object])
                    driver_link = [dcc, '-flibdcext', 'freestanding', '-fbackend', backend,
                                   *(['-fwhole-program'] if whole else []), '--gc-sections', '-e', '_start', startup_object, *[item for item in inputs if item.suffix != '.a'],
                                   *(['-L', scratch, '-lregistry', '-lunused'] if not whole else []), '-o', scratch / 'driver']
                    run(driver_link)
                    execute(scratch / 'driver', False)
                    assert 'registration' in run(['nm', scratch / 'driver'])
                if not whole:
                    control = scratch / 'without-record.exe'
                    run([linker(windows), *([] if windows else ['-no-pie']), '-Wl,--gc-sections', harness, *inputs, *libs, '-o', control])
                    assert 'registration' not in run(['nm', control])
                print('  PASS library roots', target, backend, opt, 'whole' if whole else 'separate', flush=True)
    invalid_source = scratch / 'invalid.o'
    bad_manifests = (
        ('dcc-libdcext-manifest 99\n', 'unsupported library source manifest version'),
        (manifest + 'assembly all legacy legacy.asm\n', "unknown library manifest entry 'assembly'"),
        (manifest + 'source all leaves duplicate.dc\n', "invalid or duplicate library source 'leaves'"),
        (manifest + 'source all escape ../escape.dc\n', "invalid or duplicate library source 'escape'"),
        (manifest + 'startup all entry thunk\nstartup all second thunk\n', 'duplicate library startup selection'),
    )
    for contents, diagnostic in bad_manifests:
        (include / 'libdcext.manifest').write_text(contents)
        failed([dcc, '-flibdcext', 'freestanding', '-target', target, '-I', include, '-c', entry, '-o', invalid_source], diagnostic)
        if not windows:
            failed([flag for flag in driver_link if flag != '-fwhole-program'], diagnostic)
            run(driver_link)
            execute(scratch / 'driver', False)
    (include / 'libdcext.manifest').write_text(manifest)



def assertion_roots(dcc, scratch, windows):
    source, obj = scratch / 'assert-roots.dc', scratch / 'assert-roots.o'
    target = 'x86_64-coff' if windows else 'x86_64-elf'
    cases = (
        ('public i32 probe() { return 0; }', (), False),
        ('i32 unused() { assert::__assert("unused", {}); } public i32 probe() { return 0; }', (), False),
        ('public i32 probe() { assert::__assert("explicit", {}); }', (), True),
        ('public i32 probe([]i32 values, usize index) { return values[index]; }', ('-fbounds-check',), True),
        ('using Percent = u8{0..100}; public i32 probe(i32 value) { Percent p = value as Percent; return p as i32; }', ('-frestricted-check',), True),
    )
    for backend in ('llvm', 'custom'):
        for opt in ('O0', 'O2'):
            for body, checks, needed in cases:
                source.write_text('module main; import assert;\n' + body + '\n')
                output = run([dcc, '-target', target, '-fbackend', backend, '-' + opt,
                              '-fwhole-program', '-e', 'probe', '--print-reachability',
                              *checks, '-c', source, '-o', obj])
                kept = any(line.startswith('reachability: function ') and '.__assert' in line for line in output.splitlines())
                assert kept == needed, (backend, opt, body, output)
                if opt == 'O0':
                    assert ('__assert' in run(['nm', '--defined-only', obj])) == needed
    print('  PASS assertion roots follow reachable explicit, bounds and restricted checks', target, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dcc-library-roots-') as temporary:
        scratch = Path(temporary)
        assertion_roots(args.dcc.resolve(), scratch, args.windows)
        native_mains(args.dcc.resolve(), scratch, args.windows)
        graph(args.dcc.resolve(), scratch, args.windows)
    print('  RESULT library main selection, assembly edges, lazy leaves and record retention passed')


if __name__ == '__main__':
    main()
