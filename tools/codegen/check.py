#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import measure


def main():
    parser = argparse.ArgumentParser(description='Calibrate instruction callbacks and check retained measurement artifacts.')
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    report = json.loads((directory / 'measurements.json').read_text())
    for result in report['results']:
        assert 'error' not in result, result
        for function in result['functions']:
            assert function['size'] == function['static']['code_bytes']
            for scope in ('static', 'executed'):
                assert function[scope]['instructions'] == sum(function[scope][c] for c in measure.CATEGORIES)
            if result['configuration'] != 'llvm':
                assert function['frame_metadata']
    assert measure.classify({'asm': 'mov eax,0x6766', 'bytes': 'b866670000', 'real': False}, {})[1:] == (0, 0)
    assert measure.classify({'asm': 'mov eax,0x6766', 'bytes': '66b866670000', 'real': True}, {})[1:] == (1, 0)
    for real in (True, False):
        work = directory / ('calibrate16' if real else 'calibrate64')
        work.mkdir(exist_ok=True)
        source = work / 'main.asm'
        if real:
            source.write_text('bits 16\nsection .start progbits alloc exec nowrite\nglobal _start\n_start:\n mov cx,5\n xor ax,ax\nagain:\n inc ax\n loop again\n mov di,buffer\n mov cx,4\n rep stosb\n xor ax,ax\n mov dx,0xf4\n out dx,al\nsection .data\nbuffer: times 4 db 0\n')
            measure.run(['nasm', '-f', 'elf32', source, '-o', work / 'program.o'])
            binary = work / 'program.bin'
            measure.run([measure.ROOT / 'build/bin/dcc', '-target', 'i8086-binary', '-fbase', '1000:0000', work / 'program.o', '-o', binary])
            instructions = measure.disassemble(binary, real=True, base=0x10000)
        else:
            source.write_text('bits 64\nsection .text\nglobal _start\n_start:\n mov ecx,5\n xor eax,eax\nagain:\n inc eax\n loop again\n lea rdi,[rel buffer]\n mov ecx,4\n rep stosb\n xor edi,edi\n mov eax,60\n syscall\nsection .data\nbuffer: times 4 db 0\n')
            measure.run(['nasm', '-f', 'elf64', source, '-o', work / 'program.o'])
            binary = work / 'program.elf'
            measure.run(['ld', work / 'program.o', '-o', binary])
            instructions = measure.disassemble(binary)
        functions = [{'instructions': instructions}]
        measure.execute(work, binary, functions, 'small' if real else 'llvm', directory / 'count.so', 0, 30)
        inc = [i for i in instructions if i['asm'].startswith('inc ')][0]
        loop = [i for i in instructions if i['asm'].startswith('loop ')][0]
        rep = [i for i in instructions if i['asm'].startswith('rep ')][0]
        assert inc['executions'] == 5 and loop['executions'] == 5, (inc, loop)
        print('16-bit' if real else '64-bit', 'loop body=5 loop branch=5 REP(4 bytes) callbacks=', rep['executions'])
    print('PASS artifact accounting, prefix decoding, per-instruction callbacks')


if __name__ == '__main__':
    main()
