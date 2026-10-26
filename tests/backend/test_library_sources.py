#!/usr/bin/env python3
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile
from test_memory import run

ROOT = Path(__file__).resolve().parents[2]


def execute(exe, windows):
    result = subprocess.run(['timeout', '60', *([os.environ.get('WINE', 'wine')] if windows else []), str(exe), 'one', 'two'],
                            capture_output=True, text=True, cwd=exe.parent, timeout=65)
    assert result.returncode == 0, (exe, result.returncode, result.stdout, result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    parser.add_argument('--windows', action='store_true')
    args = parser.parse_args()
    original = args.dcc.resolve()
    target, variant = ('x86_64-coff', 'windows') if args.windows else ('x86_64-elf', 'linux')
    with tempfile.TemporaryDirectory(prefix='dcc-source-library-') as temporary:
        root = Path(temporary)
        (root / 'bin').mkdir()
        dcc = root / 'bin/dcc'
        try:
            os.link(original, dcc)
        except OSError:
            shutil.copy2(original, dcc)
        shutil.copytree(original.parent.parent / 'include', root / 'include')
        assert not (root / 'lib').exists()
        source = root / 'app.dc'
        source.write_text('''module source_only_application;
import core::target;
@nomangle extern void* memset(void* destination, i32 value, usize size);
public i32 main([] const char* args) {
    static if core::target::OS == core::target::Os::Windows {
        if args.len != 0 { return 1; }
    } else {
        if args.len != 3 { return 2; }
    }
    u8[8192] bytes = {};
    memset(&bytes[0] as void*, 7, 8192);
    return if bytes[0] == 7 && bytes[8191] == 7 { 0 } else { 3 };
}
''')
        for backend in ('llvm', 'custom'):
            for opt in ('O0', 'O2'):
                obj, exe = root / 'app.o', root / ('app.exe' if args.windows else 'app')
                command = [dcc, '-target', target, '-flibdcext', variant, '-fbackend', backend, '-' + opt]
                reach = run([*command, '-fwhole-program', '--print-reachability', '-c', source, '-o', obj])
                symbols = run(['nm', '--defined-only', obj])
                for symbol in ('_start', '__dc_main', '__dc_rt_init', 'memset'):
                    assert symbol in symbols, (symbol, symbols)
                assert '__thread_entry' not in symbols and 'thread_child_main' not in symbols, symbols
                assert 'reachability:' in reach
                if args.windows:
                    cc = Path(os.environ.get('MINGW_SYSROOT', '/opt/llvm-mingw')) / 'bin/x86_64-w64-mingw32-clang'
                    run([cc, '-nostdlib', '-Wl,--entry,_start', '-Wl,--gc-sections', '-Wl,--subsystem,console', obj,
                         '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe])
                else:
                    run([*command[:1], '-flibdcext', variant, '-fbackend', backend, '-fwhole-program', '--gc-sections', obj, '-o', exe])
                assert '__dcc_libdcext_abi_requires_float_enabled' in run(['nm', '--defined-only', exe])
                execute(exe, args.windows)
                if not args.windows:
                    run([*command, '-fwhole-program', source, '-o', exe])
                    execute(exe, False)
                print('  PASS installed source-only library', variant, backend, opt, flush=True)
        if not args.windows:
            for backend in ('llvm', 'custom'):
                own = root / 'own-helper'
                run([dcc, '-flibdcext', 'freestanding', '-fwhole-program', '-e', '_start', '-fbackend', backend,
                     '-O2', ROOT / 'tests/backend/library_own_helper.dc', '-o', own])
                execute(own, False)
                symbols = run(['nm', '--defined-only', own])
                assert len([line for line in symbols.splitlines() if line.endswith(' T memcpy')]) == 1, symbols
            invalid = subprocess.run(['timeout', '60', str(dcc), '-target', 'i8086-binary', '-flibdcext', 'linux',
                                      '-fbackend', 'custom', str(source), '-o', str(root / 'dos.bin')], capture_output=True, text=True, timeout=65)
            assert invalid.returncode == 1 and 'not available for target' in invalid.stderr and 'archive' not in invalid.stderr, invalid.stderr
        if not args.windows:
            manifest_path = root / 'include/libdcext.manifest'
            saved_manifest = manifest_path.read_text()
            prelude_path = root / 'include/std/prelude.dc'
            saved_prelude = prelude_path.read_text()
            assert_path = root / 'include/assert.dc'
            saved_assert = assert_path.read_text()
            manifest_path.write_text('dcc-libdcext-manifest 1\nsource all assert assert.dc\nsource all std::prelude std/prelude.dc\nsource freestanding mock_startup startup.dc\nstartup freestanding _start dcc_main\n')
            prelude_path.write_text('module std::prelude;\n')
            assert_path.write_text('module assert; public import core::source_location; @nomangle @noreturn public extern void __assert([] const char expression, core::source_location::SourceLocation location);\n' +
                                   'asm @[intel] { ".text\\n.globl __assert\\n__assert:\\nmov al, 1\\njmp dcc_exit\\n" };\n')
            (root / 'include/startup.dc').write_text('module mock_startup;\n' + (ROOT / 'tests/i8086/crt0.dc').read_text())
            portable = root / 'portable.dc'
            portable.write_text('module portable_app; @nomangle public u16 dcc_main() { return 0; }\n')
            for model in ('small', 'unreal', 'unreal32'):
                image = root / 'portable.bin'
                run([dcc, '-target', 'i8086-binary', '-flibdcext', 'freestanding', '-fbackend', 'custom', '-mcmodel=' + model,
                     '-e', '_start', portable, '-o', image])
                run(['python3', ROOT / 'tests/i8086/run.py', image, '--mode', 'real' if model == 'small' else 'unreal'])
            manifest_path.write_text(saved_manifest)
            prelude_path.write_text(saved_prelude)
            assert_path.write_text(saved_assert)
        if args.windows:
            harness = root / 'freestanding.c'
            harness.write_text('extern int freestanding_probe(void); int main(void) { return freestanding_probe(); }\n')
            free_source = root / 'freestanding.dc'
            free_source.write_text('''module free_coff;
@nomangle extern void* memset(void* destination, i32 value, usize size);
@nomangle extern void* memcpy(void* destination, const void* source, usize size);
@nomangle extern i32 memcmp(const void* left, const void* right, usize size);
@nomangle public i32 freestanding_probe() {
    u8[32] first = {};
    u8[32] second = {};
    memset(&first[0] as void*, 7, 32);
    memcpy(&second[0] as void*, &first[0] as const void*, 32);
    return if second[31] == 7 && memcmp(&first[0] as const void*, &second[0] as const void*, 32) == 0 { 0 } else { 1 };
}
''')
            for backend in ('llvm', 'custom'):
                obj, exe = root / 'free.o', root / 'free.exe'
                run([dcc, '-target', 'x86_64-coff', '-flibdcext', 'freestanding', '-fwhole-program', '-e', 'freestanding_probe',
                     '-fno-simd', '-fno-x87', '-fbackend', backend, '-O2', '-c', free_source, '-o', obj])
                run([cc, harness, obj, '-o', exe])
                execute(exe, True)
        manifest_path = root / 'include/libdcext.manifest'
        saved_manifest = manifest_path.read_text()
        manifest_path.write_text(saved_manifest + 'source all library_injected library_injected.dc\n')
        (root / 'include/library_injected.dc').write_text('module library_injected; @nomangle public i32 injected_library_value() { return GRAPH_OPTION; }\n')
        injected_source = root / 'injected.dc'
        injected_source.write_text('module injected_application; @nomangle extern i32 injected_library_value(); public i32 main() { return injected_library_value(); }\n')
        obj, exe = root / 'injected.o', root / ('injected.exe' if args.windows else 'injected')
        run([dcc, '-target', target, '-flibdcext', variant, '-fwhole-program', '-fbackend', 'custom',
             '-J', 'public const i32 GRAPH_OPTION = 0;', '-c', injected_source, '-o', obj])
        if args.windows:
            run([cc, '-nostdlib', '-Wl,--entry,_start', '-Wl,--subsystem,console', obj,
                 '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe])
        else:
            run([dcc, '-flibdcext', variant, '-fwhole-program', '-fbackend', 'custom', obj, '-o', exe])
        execute(exe, args.windows)
        manifest_path.write_text(saved_manifest)
        abi_source = root / 'include/common/abi.dc'
        saved = abi_source.read_text()
        abi_source.write_text('module wrong_library_module;\n')
        invalid = subprocess.run(['timeout', '60', str(dcc), '-target', target, '-flibdcext', variant, '-fwhole-program',
                                  '-fbackend', 'custom', '-c', str(source), '-o', str(root / 'invalid.o')], capture_output=True, text=True, timeout=65)
        assert invalid.returncode == 1 and "expected 'dcext_abi'" in invalid.stderr, invalid.stderr
        abi_source.write_text(saved)
        (root / 'lib').symlink_to(original.parent.parent / 'lib', target_is_directory=True)
        for folder in ('common', 'linux', 'windows'):
            shutil.rmtree(root / 'include' / folder)
        for backend in ('llvm', 'custom'):
            obj, exe = root / 'separate.o', root / ('separate.exe' if args.windows else 'separate')
            run([dcc, '-target', target, '-flibdcext', variant, '-fbackend', backend, '-O2', '-c', source, '-o', obj])
            if args.windows:
                run([cc, '-nostdlib', '-Wl,--entry,_start', '-Wl,--undefined,_start', '-Wl,--subsystem,console', obj,
                     root / 'lib' / ('libdcext-windows-' + backend + '.a'), '-lkernel32', '-lws2_32', '-ladvapi32', '-lshell32', '-o', exe])
            else:
                run([dcc, '-flibdcext', variant, '-fbackend', backend, obj, '-o', exe])
            execute(exe, args.windows)
    print('  RESULT installed source-only and archive-only runtime consumers passed')


if __name__ == '__main__':
    main()
