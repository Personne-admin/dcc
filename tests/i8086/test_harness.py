#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run as runner

PROGRAMS = HERE / "programs"
NASM = os.environ.get("NASM", "nasm")
checks = 0


def assemble(directory, name, **defines):
    out = Path(directory) / ("%s-%s.bin" % (name, "-".join("%s%s" % item for item in sorted(defines.items()))))
    command = [NASM, "-f", "bin", "-I", str(HERE) + "/", str(PROGRAMS / (name + ".asm")), "-o", str(out)]
    for key, value in defines.items():
        command.append("-D%s=%s" % (key, value))
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return out.read_bytes()


def check(condition, message):
    global checks
    assert condition, message
    checks += 1


def run(binary, **kwargs):
    kwargs.setdefault("timeout", 20.0)
    return runner.run(binary, **kwargs)


def test_exit_codes(directory):
    for code in (0, 1, 7, 42, 100, 123):
        for mode in ("real", "unreal"):
            result = run(assemble(directory, "exit", CODE=code), mode=mode)
            check(result.exit_code == code and not result.timed_out and result.error is None,
                  "exit %d in %s mode: %r" % (code, mode, result))


def test_serial(directory):
    result = run(assemble(directory, "serial"))
    check(result.exit_code == 0, repr(result))
    check(result.serial == b"serial ok\x00\xff\x80\r\n", repr(result.serial))


def test_timeout(directory):
    result = run(assemble(directory, "hang"), timeout=3.0)
    check(result.timed_out and result.exit_code is None, repr(result))
    check(result.serial == b"hanging\n", "serial before hang: %r" % result.serial)


def test_loader_failure_codes(directory):
    binary = assemble(directory, "exit", CODE=125)
    result = run(binary)
    check(result.exit_code == 125 and "A20" in result.error, repr(result))


def test_layout_validation(directory):
    binary = assemble(directory, "exit", CODE=0)
    for load in ("0000:0000", "0000:7c00", "9000:0000", "1000:f800"):
        try:
            runner.run(binary, load)
        except runner.HarnessError:
            check(True, load)
        else:
            check(False, "accepted %s" % load)


def test_load_addresses(directory):
    for seg, off in ((0x1000, 0x0000), (0x2345, 0x0100), (0x0800, 0x0010)):
        load = "%04x:%04x" % (seg, off)
        for mode in ("real", "unreal"):
            result = run(assemble(directory, "entry", ORG=off), load=load, mode=mode)
            expected = "CS=%04X DS=%04X ES=%04X SS=%04X FS=0000 GS=0000 SP=FFFE DF=0000 IP=%04X\n" % (seg, seg, seg, seg, off)
            check(result.exit_code == 0 and result.serial.decode() == expected,
                  "%s %s: %r" % (load, mode, result.serial))


def test_large_binary(directory):
    padding = "times 40000 db 0x90\n"
    source = Path(directory) / "large.asm"
    source.write_text('%%include "common.inc"\nstart:\n    jmp tail\n%s tail:\n    EXIT 33\n' % padding)
    out = Path(directory) / "large.bin"
    subprocess.run([NASM, "-f", "bin", "-I", str(HERE) + "/", str(source), "-o", str(out)], check=True)
    result = run(out.read_bytes(), load="3000:0230")
    check(result.exit_code == 33, repr(result))


def test_a20(directory):
    for mode in ("real", "unreal"):
        result = run(assemble(directory, "a20"), mode=mode)
        check(result.exit_code == 0 and result.serial == b"A20 on\n", repr(result))


def test_i386(directory):
    binary = assemble(directory, "i386")
    check(b"\x66" in binary, "no 0x66 operand-size prefix in the i386 program")
    check(b"\x67" in binary, "no 0x67 address-size prefix in the i386 program")
    for mode in ("real", "unreal"):
        result = run(binary, mode=mode)
        check(result.exit_code == 0 and result.serial == b"i386 ok\n", "%s: %r" % (mode, result))


def descriptor_limits(directory, mode):
    result = run(assemble(directory, "ready"), mode=mode, ready_marker="READY")
    check(result.registers and all(name in result.registers for name in ("DS", "ES", "FS", "GS", "SS", "CS")),
          "no register dump: %r" % result.registers)
    return result.registers


def test_unreal(directory):
    binary = assemble(directory, "unreal")
    result = run(binary, mode="unreal")
    check(result.exit_code == 0 and result.serial == b"unreal ok\n", repr(result))
    for load in ("1000:0000", "2000:0123"):
        result = run(binary, load=load, mode="unreal")
        check(result.exit_code == 0, "unreal at %s: %r" % (load, result))
    unreal = descriptor_limits(directory, "unreal")
    for name in ("DS", "ES", "FS", "GS"):
        check(unreal[name]["limit"] == 0xFFFFFFFF, "%s limit %x in unreal mode" % (name, unreal[name]["limit"]))
    for name in ("SS", "CS"):
        check(unreal[name]["limit"] == 0xFFFF, "%s limit %x in unreal mode" % (name, unreal[name]["limit"]))
    real = descriptor_limits(directory, "real")
    for name in ("DS", "ES", "FS", "GS", "SS", "CS"):
        check(real[name]["limit"] == 0xFFFF, "%s limit %x in real mode" % (name, real[name]["limit"]))
    check(unreal["DS"]["base"] == 0x10000 and real["DS"]["base"] == 0x10000, "ds base")
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        result = run(binary, mode="real", accel="kvm")
        check(result.exit_code == 77 and result.serial == b"unreal fault\n", "real mode must fault on a 32-bit offset under KVM: %r" % result)
        print("  NOTE    real-mode fault check ran under KVM")
    else:
        print("  NOTE    no /dev/kvm: TCG does not enforce segment limits")


with tempfile.TemporaryDirectory(prefix="i8086-harness-") as scratch:
    for test in (test_exit_codes, test_serial, test_timeout, test_loader_failure_codes, test_layout_validation,
                 test_load_addresses, test_large_binary, test_a20, test_i386, test_unreal):
        test(scratch)
print("  RESULT  i8086 qemu harness: %d/%d passed" % (checks, checks))
