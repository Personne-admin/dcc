#!/usr/bin/env python3
import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOADER = HERE / "loader.asm"

EXIT_PORT = 0xF4
SERIAL_FILE_ARG = "file:"
SECTOR = 512
LOADER_ERRORS = {124: "loader: disk read failed", 125: "loader: A20 line could not be enabled"}
FIRST_RESERVED_EXIT = 124
STATUS_TIMEOUT = 254
STATUS_HARNESS = 255
MIN_LINEAR = 0x8000
MAX_STACK_TOP = 0x9FC00
MIN_STACK = 0x1000

SEGMENT_REGISTER = re.compile(r"^(ES|CS|SS|DS|FS|GS)\s*=([0-9a-fA-F]{4}) ([0-9a-fA-F]+) ([0-9a-fA-F]+) ([0-9a-fA-F]+)", re.M)


class HarnessError(Exception):
    pass


@dataclass
class Result:
    exit_code: "int | None"
    serial: bytes
    timed_out: bool
    qemu_status: "int | None"
    registers: "dict | None" = None
    error: "str | None" = None


def parse_load(text):
    try:
        seg, off = text.split(":")
        seg, off = int(seg, 16), int(off, 16)
    except ValueError:
        raise HarnessError("load address must be SEGMENT:OFFSET in hex, got %r" % text)
    if not (0 <= seg <= 0xFFFF and 0 <= off <= 0xFFFF):
        raise HarnessError("load address out of range: %r" % text)
    return seg, off


def check_layout(seg, off, size):
    if size == 0:
        raise HarnessError("empty binary")
    if seg * 16 + off < MIN_LINEAR:
        raise HarnessError("load address %04X:%04X is below linear 0x%X (loader area)" % (seg, off, MIN_LINEAR))
    if seg * 16 + 0x10000 > MAX_STACK_TOP:
        raise HarnessError("segment %04X puts the stack top (SS:FFFE) above linear 0x%X" % (seg, MAX_STACK_TOP))
    if off + size > 0x10000 - MIN_STACK:
        raise HarnessError("binary of %d bytes at offset %04X leaves less than %d bytes of stack" % (size, off, MIN_STACK))


def build_image(binary, seg, off, mode, nasm="nasm", workdir=None):
    if mode not in ("real", "unreal"):
        raise HarnessError("mode must be real or unreal")
    check_layout(seg, off, len(binary))
    sectors = (len(binary) + SECTOR - 1) // SECTOR
    with tempfile.TemporaryDirectory(prefix="i8086-loader-", dir=workdir) as directory:
        out = Path(directory) / "loader.bin"
        command = [nasm, "-f", "bin", "-w+all", "-DLOAD_SEG=%d" % seg, "-DLOAD_OFF=%d" % off,
                   "-DSECTORS=%d" % sectors, "-DUNREAL=%d" % (mode == "unreal"), str(LOADER), "-o", str(out)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            raise HarnessError("nasm failed on the loader:\n" + result.stderr)
        boot = out.read_bytes()
    assert len(boot) == SECTOR
    return boot + binary + bytes(sectors * SECTOR - len(binary))


def parse_registers(text):
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
    registers = {}
    for match in SEGMENT_REGISTER.finditer(text):
        name, selector, base, limit, flags = match.groups()
        registers[name] = {"selector": int(selector, 16), "base": int(base, 16),
                           "limit": int(limit, 16), "flags": int(flags, 16)}
    return registers


def run(binary, load="1000:0000", mode="real", timeout=10.0, qemu="qemu-system-i386", nasm="nasm",
        accel="tcg", memory_mb=64, cpu=None, ready_marker=None, workdir=None):
    seg, off = parse_load(load) if isinstance(load, str) else load
    image = build_image(binary, seg, off, mode, nasm, workdir)
    with tempfile.TemporaryDirectory(prefix="i8086-run-", dir=workdir) as directory:
        disk = Path(directory) / "disk.img"
        serial_path = Path(directory) / "serial.out"
        disk.write_bytes(image)
        serial_path.write_bytes(b"")
        command = [qemu, "-nodefaults", "-display", "none", "-no-reboot", "-accel", accel,
                   "-m", str(memory_mb), "-drive", "file=%s,format=raw,if=ide" % disk,
                   "-serial", SERIAL_FILE_ARG + str(serial_path),
                   "-device", "isa-debug-exit,iobase=0x%x,iosize=0x04" % EXIT_PORT,
                   "-monitor", "stdio" if ready_marker else "none"]
        if cpu:
            command += ["-cpu", cpu]
        try:
            process = subprocess.Popen(command, stdin=subprocess.PIPE if ready_marker else subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        except FileNotFoundError:
            raise HarnessError("%s not found; install qemu-system-x86" % qemu)
        timed_out = False
        registers = None
        stdout = b""
        stderr = b""
        deadline = time.monotonic() + timeout
        try:
            if ready_marker:
                marker = ready_marker.encode()
                while time.monotonic() < deadline and process.poll() is None:
                    if marker in serial_path.read_bytes():
                        process.stdin.write(b"info registers\n")
                        process.stdin.flush()
                        time.sleep(0.3)
                        process.stdin.write(b"quit\n")
                        process.stdin.flush()
                        break
                    time.sleep(0.02)
            remaining = max(deadline - time.monotonic(), 0.1)
            stdout, stderr = process.communicate(timeout=remaining)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
        serial = serial_path.read_bytes()
    if ready_marker:
        registers = parse_registers(stdout.decode(errors="replace"))
    if timed_out:
        return Result(None, serial, True, None, registers)
    status = process.returncode
    stderr_text = stderr.decode(errors="replace").strip()
    if status % 2 == 0 or (status == 1 and stderr_text):
        return Result(None, serial, False, status, registers,
                      stderr_text or "qemu exited with status %d without the debug-exit device firing" % status)
    code = status >> 1
    error = LOADER_ERRORS.get(code, "reserved exit code %d" % code) if code >= FIRST_RESERVED_EXIT else None
    return Result(code, serial, False, status, registers, error)


def main(argv):
    parser = argparse.ArgumentParser(description="Run a flat 16-bit binary under qemu-system-i386.")
    parser.add_argument("binary", type=Path)
    parser.add_argument("--load", default="1000:0000", help="SEGMENT:OFFSET in hex")
    parser.add_argument("--mode", choices=("real", "unreal"), default="real")
    parser.add_argument("--timeout", type=float, default=10.0, help="seconds before the guest is killed")
    parser.add_argument("--serial-out", type=Path, help="also write the captured serial bytes to this file")
    parser.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-i386"))
    parser.add_argument("--nasm", default=os.environ.get("NASM", "nasm"))
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--cpu")
    args = parser.parse_args(argv)
    try:
        result = run(args.binary.read_bytes(), args.load, args.mode, args.timeout, args.qemu, args.nasm,
                     args.accel, cpu=args.cpu)
    except HarnessError as error:
        print("run.py: %s" % error, file=sys.stderr)
        return STATUS_HARNESS
    if args.serial_out:
        args.serial_out.write_bytes(result.serial)
    sys.stdout.buffer.write(result.serial)
    sys.stdout.buffer.flush()
    if result.timed_out:
        print("run.py: timed out after %gs" % args.timeout, file=sys.stderr)
        return STATUS_TIMEOUT
    if result.error:
        print("run.py: %s" % result.error, file=sys.stderr)
        return STATUS_HARNESS
    return result.exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
