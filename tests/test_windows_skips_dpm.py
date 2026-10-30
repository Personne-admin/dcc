import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKS = 0
FAILURES = []


def check(ok, label, detail=""):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAILURES.append(label)
        print("FAIL  " + label)
        if detail:
            print(detail)


def make(*args):
    return subprocess.run(["make", "-C", ROOT, *args], capture_output=True, text=True, timeout=120)


def prerequisites(target, *args):
    r = make("-qp", "ENABLE_LLVM=0", *args)
    for line in r.stdout.splitlines():
        if line.startswith(target + ":") and not line.startswith(target + "::"):
            return line.split(":", 1)[1].split()
    return None


native = prerequisites("dccd")
check(native is not None and "dpm" in native, "a native dccd build needs dpm", str(native))
cross = prerequisites("dccd", "CROSS=windows")
check(cross is not None and "compiler" in cross and "dpm" not in cross, "a Windows dccd build does not build dpm", str(cross))
plain = prerequisites("dccd", "DCCD_DCON=0")
check(plain is not None and "dpm" not in plain, "DCCD_DCON=0 builds dccd without dpm", str(plain))
dpm_cross = prerequisites("dpm", "CROSS=windows")
check(dpm_cross == [], "dpm has no prerequisites in a Windows build", str(dpm_cross))
dpm_native = prerequisites("dpm")
check(dpm_native == ["compiler"], "dpm is built after the compiler natively", str(dpm_native))

r = make("CROSS=windows", "ENABLE_LLVM=0", "dpm")
check(r.returncode == 0 and "POSIX only" in r.stdout and "skipped" in r.stdout, "the dpm target says why it is skipped", r.stdout + r.stderr)
check(not os.path.exists(os.path.join(ROOT, "build-windows", "bin", "dpm.exe")), "no dpm.exe is produced")

tools = prerequisites("tools-windows", "CROSS=windows")
check(tools is not None, "tools-windows exists", str(tools))
text = open(os.path.join(ROOT, "GNUmakefile")).read()
recipe = text.split("tools-windows:\n", 1)[1].split("\n\n", 1)[0]
check("CROSS=windows driver" in recipe and "CROSS=windows dccd" in recipe and "CROSS=windows dpm" in recipe, "tools-windows builds the driver and dccd and reports dpm as skipped", recipe)
msi = text.split("msi: tools-windows\n", 1)[1].split("\n\n", 1)[0]
check("dpm" not in msi, "the installer stages no dpm", msi)

dccd_make = open(os.path.join(ROOT, "dccd", "GNUmakefile")).read()
check("DCCD_DCON ?= $(if $(filter windows,$(CROSS)),0,1)" in dccd_make, "dccd omits dcon analysis by default in Windows builds")
check("BASE_CXXFLAGS += -DDCCD_NO_DCON" in dccd_make and "libdpmcore.a" in dccd_make, "the dpm library is only linked when dcon analysis is built")

print("%d checks, %d failed" % (CHECKS, len(FAILURES)))
sys.exit(1 if FAILURES else 0)
