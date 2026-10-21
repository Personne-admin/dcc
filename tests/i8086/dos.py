import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

import mz
import run as bare

HERE = Path(__file__).resolve().parent


@dataclass
class Result:
    exit_code: int | None
    stdout: bytes
    stderr: bytes
    timed_out: bool
    error: str | None


def assemble(source, output, defines, nasm):
    command = [nasm, '-f', 'bin', '-w+all', '-I' + str(HERE) + '/']
    command += ['-D%s=%s' % item for item in defines.items()]
    process = subprocess.run(command + [str(source), '-o', str(output)], capture_output=True, timeout=60)
    if process.returncode:
        raise bare.HarnessError(process.stderr.decode(errors='replace'))
    return output.read_bytes()


def build_image(binary, format, paragraph=0x3000, command_tail=b'', nasm='nasm'):
    if not 0x2000 <= paragraph <= 0x8000:
        raise bare.HarnessError('DOS load paragraph must be between 2000h and 8000h')
    if len(command_tail) > 126 or b'\r' in command_tail:
        raise bare.HarnessError('invalid PSP command tail')
    if format == 'mz':
        try:
            parsed = mz.parse(binary)
        except ValueError as error:
            raise bare.HarnessError(str(error)) from error
        memory = parsed.memory_size
        psp = paragraph - 16
        offset = 0
        if parsed.header['cparhdr'] * 16 > 0x8000:
            raise bare.HarnessError('test loader header exceeds staging window')
    elif format == 'com':
        if not binary or len(binary) > 0xef00:
            raise bare.HarnessError('COM image leaves less than 4096 bytes of stack')
        psp = paragraph
        memory = 0x10000
        offset = 0x100
    else:
        raise bare.HarnessError('format must be com or mz')
    if paragraph * 16 + memory > 0x90000:
        raise bare.HarnessError('DOS allocation exceeds test conventional memory')
    with tempfile.TemporaryDirectory(prefix='dcc-dos-') as directory:
        directory = Path(directory)
        image_file, tail_file = directory / 'image.bin', directory / 'tail.bin'
        image_file.write_bytes(binary)
        tail_file.write_bytes(command_tail + b'\r')
        defines = dict(LOAD_SEG=paragraph, PSP_SEG=psp, END_SEG=paragraph + (memory + 15)//16,
                       MEMORY_PARAS=(memory + 15)//16, IMAGE_OFF=offset, IMAGE_SIZE=len(binary),
                       IS_MZ=int(format == 'mz'), TAIL_SIZE=len(command_tail),
                       IMAGE_FILE='"%s"' % image_file, TAIL_FILE='"%s"' % tail_file)
        stage = assemble(HERE / 'dos_loader.asm', directory / 'stage.bin', defines, nasm)
        if 0x8000 + len(stage) > (psp * 16):
            raise bare.HarnessError('DOS allocation overlaps loader staging area')
        sectors = (len(stage) + 511) // 512
        boot = assemble(bare.LOADER, directory / 'boot.bin',
                        dict(LOAD_SEG=0x800, LOAD_OFF=0, SECTORS=sectors, UNREAL=0), nasm)
        return boot + stage + bytes(sectors * 512 - len(stage))


def decode(result):
    stdout, stderr = bytearray(), bytearray()
    if result.timed_out:
        return Result(None, b'', b'', True, 'QEMU DOS timeout')
    stream = result.serial
    error = result.error
    exit_code = None
    if len(stream) % 2:
        error = 'truncated DOS serial record'
    else:
        for index in range(0, len(stream), 2):
            tag, value = stream[index:index+2]
            if exit_code is not None:
                error = 'records after DOS completion'
                break
            if tag in (1, 2):
                (stdout if tag == 1 else stderr).append(value)
            elif tag == 3:
                exit_code = value
            elif tag == 4:
                error = 'unsupported DOS service AH=%02x' % value
                break
            else:
                error = 'unknown DOS serial record'
                break
    if result.qemu_status != 1:
        error = error or 'unexpected QEMU DOS completion status'
    if exit_code is None:
        error = error or 'missing DOS completion record'
    return Result(exit_code if not error else None, bytes(stdout), bytes(stderr), False, error)


def run(binary, format, paragraph=0x3000, command_tail=b'', timeout=10, nasm='nasm', **kwargs):
    image = build_image(binary, format, paragraph, command_tail, nasm)
    return decode(bare.run_image(image, timeout=timeout, **kwargs))
