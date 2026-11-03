import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

DPM = os.path.abspath(sys.argv[1])
DCC = os.path.abspath(sys.argv[2])
TASKS = sys.argv[3:] or ["run", "run-uefi", "run-hdd", "run-hdd-uefi"]
HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "templates", "limine-x86-64")
LIMINE = os.environ.get("DPM_TEST_LIMINE", "")
OVMF = os.environ.get("DPM_TEST_OVMF", "")
WAIT = float(os.environ.get("DPM_BOOT_WAIT", "8"))
FAILURES = []


def check(ok, label, detail=""):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        FAILURES.append(label)
        if detail:
            print(detail)


def lit_pixels(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        return 0, 0
    width, height = int(m.group(1)), int(m.group(2))
    pixels = data[m.end():]
    lit = sum(1 for i in range(0, len(pixels) - 2, 3) if pixels[i] or pixels[i + 1] or pixels[i + 2])
    return lit, width * height


def monitor(sock_path, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock_path)
        s.settimeout(5)
        time.sleep(0.3)
        s.sendall(command.encode() + b"\n")
        time.sleep(1.0)


def main():
    if not LIMINE or not os.path.isfile(os.path.join(LIMINE, "limine.c")):
        print("set DPM_TEST_LIMINE to an extracted limine-binary directory (and DPM_TEST_OVMF for the UEFI tasks)")
        return 2
    root = tempfile.mkdtemp(prefix="dpm_boot_")
    env = dict(os.environ)
    env.update({"DPM_HOME": os.path.join(root, "home"), "DCC": DCC, "DPM_REGISTRY": ""})
    try:
        r = subprocess.run([DPM, "new", "kernel", "--template", "path:" + TEMPLATE, "--set", "title=Boot Test"], cwd=root, env=env, capture_output=True, text=True)
        check(r.returncode == 0 and "nothing was run" in r.stdout, "dpm new from the checked-in template", r.stdout + r.stderr)
        app = os.path.join(root, "kernel")
        manifest = os.path.join(app, "project.dcon")
        text = open(manifest).read()
        text = re.sub(r'\{ name = "limine", location = "[^"]*", version = "[^"]*" \}', '{ name = "limine", location = "path:%s" }' % LIMINE, text)
        if OVMF:
            text = re.sub(r'\{ name = "ovmf", location = "[^"]*", version = "[^"]*", max_size = \d+ \}', '{ name = "ovmf", location = "path:%s" }' % OVMF, text)
        open(manifest, "w").write(text)
        for task in TASKS:
            if "uefi" in task and not OVMF:
                print("skip  %s (no DPM_TEST_OVMF)" % task)
                continue
            sock = os.path.join(root, task + ".sock")
            shot = os.path.join(root, task + ".ppm")
            args = [DPM, "task", task, "--", "-m", "256M", "-display", "none", "-no-reboot", "-monitor", "unix:%s,server,nowait" % sock]
            proc = subprocess.Popen(args, cwd=app, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = time.time() + 600
            while not os.path.exists(sock) and proc.poll() is None and time.time() < deadline:
                time.sleep(0.2)
            if not os.path.exists(sock):
                out = proc.communicate(timeout=60)[0] if proc.poll() is not None else ""
                check(False, "%s starts QEMU" % task, out)
                proc.kill()
                continue
            time.sleep(WAIT)
            monitor(sock, "screendump " + shot)
            monitor(sock, "quit")
            try:
                out = proc.communicate(timeout=60)[0]
            except subprocess.TimeoutExpired:
                proc.kill()
                out = proc.communicate()[0]
            lit, total = lit_pixels(shot) if os.path.exists(shot) else (0, 0)
            check(total > 0 and lit * 10 > total * 9, "%s boots the kernel (%d of %d pixels lit)" % (task, lit, total), out[-2000:])
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("%d failed" % len(FAILURES))
    return 1 if FAILURES else 0


sys.exit(main())
