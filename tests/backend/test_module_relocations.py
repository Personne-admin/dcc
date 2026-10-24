#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import struct
import subprocess
import tempfile
from test_memory import run

MODIFIERS = ('GOTPCREL', 'GOTPCRELX', 'REX_GOTPCRELX', 'PLT', 'GOT', 'GOTOFF', 'GOTPC',
             'TPOFF', 'DTPOFF', 'TLSGD', 'TLSLD', 'GOTTPOFF', 'TLSDESC', 'UNKNOWN_RELOCATION')


def elf_records(obj):
    relocations = []
    for line in run(['readelf', '-Wr', obj]).splitlines():
        match = re.match(r'\s*([0-9a-f]+)\s+[0-9a-f]+\s+(R_X86_64_\w+)\s+[0-9a-f]+\s+(\S+)\s+([+-])\s+([0-9a-f]+)', line)
        if match and match[3] in ('external_data', 'external_call', '__imp_ExitProcess'):
            relocations.append((match[2], match[3], int(match[5], 16) * (-1 if match[4] == '-' else 1)))
    symbols = run(['readelf', '-Ws', obj])
    for name in ('external_data', 'external_call', '__imp_ExitProcess'):
        assert re.search(r'NOTYPE\s+GLOBAL\s+DEFAULT\s+UND\s+' + name + r'$', symbols, re.M), symbols
    for name in ('relocation_entry', 'relocation_words'):
        assert re.search(r'NOTYPE\s+GLOBAL\s+DEFAULT\s+\d+\s+' + name + r'$', symbols, re.M), symbols
    return sorted(relocations)


def coff_records(obj):
    dump = run(['objdump', '-r', '-t', obj])
    for name in ('external_data', 'external_call', '__imp_ExitProcess'):
        assert re.search(r'\(sec\s+0\).*\(scl\s+2\).*\b' + name + r'$', dump, re.M), dump
    for name in ('relocation_entry', 'relocation_words'):
        assert re.search(r'\(sec\s+[1-9]\d*\).*\(scl\s+2\).*\b' + name + r'$', dump, re.M), dump
    data = obj.read_bytes()
    machine, sections, _, sympos, symcount, optional, _ = struct.unpack_from('<HHIIIHH', data)
    assert machine == 0x8664 and optional == 0
    strings = sympos + symcount * 18

    def name_at(index):
        entry = data[sympos + index * 18:sympos + index * 18 + 8]
        if entry[:4] == b'\0' * 4:
            start = strings + struct.unpack_from('<I', entry, 4)[0]
            return data[start:data.index(b'\0', start)].decode()
        return entry.rstrip(b'\0').decode()

    relocations = []
    kinds = {1: ('IMAGE_REL_AMD64_ADDR64', '<q'), 4: ('IMAGE_REL_AMD64_REL32', '<i')}
    for index in range(sections):
        base = 20 + index * 40
        rawpos, relpos = struct.unpack_from('<II', data, base + 20)
        count = struct.unpack_from('<H', data, base + 32)[0]
        for item in range(count):
            offset, symbol, kind = struct.unpack_from('<IIH', data, relpos + item * 10)
            name = name_at(symbol)
            if name in ('external_data', 'external_call', '__imp_ExitProcess'):
                label, encoding = kinds[kind]
                relocations.append((label, name, struct.unpack_from(encoding, data, rawpos + offset)[0]))
    return sorted(relocations)


def reject(dcc, scratch, target, opt, whole):
    source = scratch / 'rejected.dc'
    flags = [dcc, '-target', target, '-fbackend', 'custom', '-' + opt,
             *(['-fwhole-program', '-e', 'probe'] if whole else []), '-c', source, '-o', scratch / 'rejected.o']
    cases = [(f'.text\nmov sym@{modifier}(%rip), %rax', f"relocation modifier '@{modifier}' is not supported by the custom backend") for modifier in MODIFIERS]
    cases.append(('.weak missing', "weak symbol 'missing' must be defined in module asm"))
    if target == 'x86_64-coff':
        cases.append(('.text\n.weak weak_defined\nweak_defined:\nret', 'module asm weak symbol `weak_defined` is not supported for COFF targets'))
    for assembly, diagnostic in cases:
        escaped = assembly.replace('%', '%%').replace('\n', '\\n')
        source.write_text('module rejected;\n@nomangle public i32 probe() { return 0; }\nasm { "' + escaped + '" };\n')
        result = subprocess.run(['timeout', '60', *map(str, flags)], capture_output=True, text=True, timeout=65)
        assert result.returncode == 1 and diagnostic in result.stderr, (flags, diagnostic, result.returncode, result.stdout, result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dcc', type=Path)
    args = parser.parse_args()
    dcc = args.dcc.resolve()
    source = Path(__file__).with_name('module_relocations.dc')
    with tempfile.TemporaryDirectory(prefix='dcc-module-relocations-') as temp:
        scratch = Path(temp)
        for target, inspect, expected in (
            ('x86_64-elf', elf_records, [('R_X86_64_PC32', 'external_data', 4), ('R_X86_64_PLT32', 'external_call', -4),
                                       ('R_X86_64_PC32', '__imp_ExitProcess', -4), ('R_X86_64_64', 'external_data', 13), ('R_X86_64_64', 'external_call', 17)]),
            ('x86_64-coff', coff_records, [('IMAGE_REL_AMD64_REL32', 'external_data', 8), ('IMAGE_REL_AMD64_REL32', 'external_call', 0),
                                         ('IMAGE_REL_AMD64_REL32', '__imp_ExitProcess', 0), ('IMAGE_REL_AMD64_ADDR64', 'external_data', 13), ('IMAGE_REL_AMD64_ADDR64', 'external_call', 17)])):
            for opt in ('O0', 'O2'):
                for whole in (False, True):
                    records = []
                    for backend in ('llvm', 'custom'):
                        obj = scratch / (backend + '.o')
                        run([dcc, '-target', target, '-fbackend', backend, '-' + opt,
                             *(['-fwhole-program', '-e', 'probe'] if whole else []), '-c', source, '-o', obj])
                        records.append(inspect(obj))
                    assert records[0] == records[1] == sorted(expected), (target, opt, whole, records, expected)
                    reject(dcc, scratch, target, opt, whole)
                    print('  PASS module relocations', target, opt, 'whole' if whole else 'separate', flush=True)
    print('  RESULT module relocation parity and rejection diagnostics passed')


if __name__ == '__main__':
    main()
