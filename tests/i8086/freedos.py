from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "i8086" / "freedos_launcher.asm"
IMAGE_SHA256 = "552f7cbb0625960c050a3e682a4d2121cc2ffdfa9dc9d592ccd49edf179333dc"
QEMU_EXIT = (0x10 << 1) | 1


def image_path():
    candidates = []
    env = os.environ.get("DCC_FREEDOS_IMAGE")
    if env:
        candidates.append(Path(env))
    candidates.append(Path("/usr/share/freedos/x86BOOT.img"))
    candidates.append(Path.home() / ".cache" / "dcc-freedos" / "x86BOOT.img")
    for path in candidates:
        if path.is_file():
            return path
    raise RuntimeError("FreeDOS base image not found; set DCC_FREEDOS_IMAGE")


def _sha256(path):
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class FreeDosResult:
    def __init__(self, status_kind, status_code, stdout, stderr, psp_hex):
        self.status_kind = status_kind
        self.status_code = status_code
        self.stdout = stdout
        self.stderr = stderr
        self.psp_hex = psp_hex

    @property
    def ok(self):
        return self.status_kind == "S"


def run(program, kind, mode="", timeout=30):
    program = Path(program)
    kind = kind.upper()
    if kind not in ("COM", "EXE"):
        raise ValueError(kind)
    base = image_path()
    if _sha256(base) != IMAGE_SHA256:
        raise RuntimeError(f"FreeDOS base image checksum mismatch: {base}")
    with tempfile.TemporaryDirectory(prefix="dcc-freedos-") as temporary:
        temporary = Path(temporary)
        disk = temporary / "disk.img"
        shutil.copyfile(base, disk)
        launcher = temporary / "RUNNER.COM"
        subprocess.run(["nasm", "-f", "bin", "-o", str(launcher), str(LAUNCHER)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(["mcopy", "-o", "-i", str(disk), str(launcher), "::RUNNER.COM"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(["mcopy", "-o", "-i", str(disk), str(program), f"::T.{kind}"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        command = "A:\\RUNNER.COM"
        if mode:
            command += " " + mode
        autoexec = temporary / "FDAUTO.BAT"
        autoexec.write_bytes(("@echo off\r\n" + command + "\r\n").encode("ascii"))
        subprocess.run(["mcopy", "-o", "-i", str(disk), str(autoexec), "::FDAUTO.BAT"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        qemu = [
            "qemu-system-i386", "-m", "16", "-cpu", "486",
            "-drive", f"file={disk},format=raw,if=floppy",
            "-boot", "a", "-display", "none", "-serial", "none", "-monitor", "none",
            "-no-reboot", "-device", "isa-debug-exit,iobase=0x501,iosize=0x03",
        ]
        try:
            result = subprocess.run(qemu, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError("FreeDOS run timed out") from error
        if result.returncode != QEMU_EXIT:
            raise RuntimeError(f"FreeDOS run did not complete: qemu exit {result.returncode}")
        extracted = {}
        for name in ("OUT.TXT", "ERR.TXT", "STAT.TXT"):
            target = temporary / name
            copy = subprocess.run(["mcopy", "-o", "-i", str(disk), f"::{name}", str(target)],
                                  capture_output=True)
            extracted[name] = target.read_bytes() if copy.returncode == 0 else b""
        status = extracted["STAT.TXT"]
        if len(status) < 2 or status[0] not in (ord("S"), ord("E")):
            raise RuntimeError(f"FreeDOS status record missing or malformed: {status!r}")
        return FreeDosResult(chr(status[0]), status[1], extracted["OUT.TXT"], extracted["ERR.TXT"], None)
