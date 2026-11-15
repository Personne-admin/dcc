#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DCC = Path(sys.argv[1]).resolve()
CC = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
READELF = shutil.which("llvm-readelf") or shutil.which("readelf")
failures = []
checks = 0


def check(condition, label, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(label)
        print(f"  FAIL     {label}")
        if detail:
            print("           " + detail.replace("\n", "\n           "))


def run(argv, cwd=None, env=None, timeout=120):
    merged = dict(os.environ)
    if env:
        merged.update(env)
    return subprocess.run([str(a) for a in argv], cwd=cwd, env=merged, capture_output=True, text=True, timeout=timeout)


def dcc(*args, cwd=None, env=None):
    return run([DCC, *args], cwd=cwd, env=env)


def readelf(*args):
    return run([READELF, "-W", *args]).stdout


def cbuild_shared(directory, name, source):
    directory.mkdir(parents=True, exist_ok=True)
    c_file = directory / f"{name}.c"
    c_file.write_text(source)
    library = directory / f"lib{name}.so"
    result = run([CC, "-shared", "-fPIC", "-o", library, c_file])
    if result.returncode != 0:
        raise SystemExit(f"error: cannot build the C shared library: {result.stderr}")
    return library


def test_custom_shared_library_output(root):
    out = root / "shared out" / "libplugin.so"
    out.parent.mkdir(parents=True)
    source = root / "plugin.dc"
    source.write_text("module plugin;\n\n@nomangle public i32 plugin_twice(i32 x) {\n    return x * 2;\n}\n")
    r = dcc(source, "-shared", "-fPIC", "-fbackend=custom", "-o", out)
    check(r.returncode == 0, "custom backend writes a shared library into a path with spaces", r.stderr)
    if r.returncode != 0:
        return
    check("DYN (Shared object file)" in readelf("-h", out), "the custom shared library is ET_DYN")
    user = root / "use_plugin.c"
    user.write_text("int plugin_twice(int);\nint main(void) { return plugin_twice(21) == 42 ? 0 : 1; }\n")
    exe = root / "use_plugin"
    r = run([CC, user, "-o", exe, f"-L{out.parent}", "-lplugin", f"-Wl,-rpath,{out.parent}"])
    check(r.returncode == 0, "a C program links against the dcc shared library", r.stderr)
    if r.returncode == 0:
        check(run([exe]).returncode == 0, "the C program calls into the dcc shared library")


SHIM_C = """int shim_add(int a, int b) { return a + b + 1000; }
int shim_counter = 41;
"""

CALL_DC = """module main;

@nomangle extern i32 shim_add(i32 a, i32 b);

public i32 main() {
    i32 value = shim_add(2, 3);
    println("shim_add(2, 3) = {}", value);
    return if value == 1005 { 0 } else { 1 };
}
"""

DATA_DC = """module main;

@nomangle extern i32 shim_counter;

public i32 main() {
    shim_counter = shim_counter + 1;
    println("counter {}", shim_counter);
    return if shim_counter == 42 { 0 } else { 1 };
}
"""

HELLO_DC = """module main;

public i32 main() {
    println("static hello");
    return 0;
}
"""


def interpreter(path):
    out = readelf("-l", path)
    marker = "Requesting program interpreter: "
    for line in out.splitlines():
        if marker in line:
            return line.split(marker, 1)[1].rstrip("]").strip()
    return None


def dynamic_entries(path, tag):
    return [line.split("[", 1)[1].rstrip("]") for line in readelf("-d", path).splitlines() if f"({tag})" in line and "[" in line]


def test_consume_shared_library(root):
    libdir = root / "native libs"
    library = cbuild_shared(libdir, "shim", SHIM_C)
    source = root / "call.dc"
    source.write_text(CALL_DC)
    for backend in ("llvm", "custom"):
        for opt in ("-O0", "-O2"):
            for mode in ("-fwhole-program", "-fno-whole-program"):
                label = f"{backend} {opt} {mode}"
                exe = root / f"call-{backend}{opt}{mode}"
                r = dcc(source, "-o", exe, "-flibdcext", f"-fbackend={backend}", opt, mode, f"-L{libdir}", "-lshim", "-rpath", str(libdir))
                check(r.returncode == 0, f"{label}: links against a shared library", r.stderr)
                if r.returncode != 0:
                    continue
                ran = run([exe])
                check(ran.returncode == 0 and "shim_add(2, 3) = 1005" in ran.stdout, f"{label}: calls the shared library", ran.stdout + ran.stderr)
                check(interpreter(exe) == "/lib64/ld-linux-x86-64.so.2", f"{label}: PT_INTERP is the x86_64 glibc loader", str(interpreter(exe)))
                check(dynamic_entries(exe, "NEEDED") == ["libshim.so"], f"{label}: DT_NEEDED names libshim.so", str(dynamic_entries(exe, "NEEDED")))
                check(dynamic_entries(exe, "RUNPATH") == [str(libdir)], f"{label}: DT_RUNPATH records the -rpath directory", str(dynamic_entries(exe, "RUNPATH")))
                check("EXEC (Executable file)" in readelf("-h", exe), f"{label}: the executable is position dependent (ET_EXEC)")

    data = root / "data.dc"
    data.write_text(DATA_DC)
    for backend in ("llvm", "custom"):
        exe = root / f"data-{backend}"
        r = dcc(data, "-o", exe, "-flibdcext", f"-fbackend={backend}", "-fwhole-program", f"-L{libdir}", "-lshim", "-rpath", str(libdir))
        check(r.returncode == 0, f"{backend}: links a shared library data symbol", r.stderr)
        if r.returncode == 0:
            ran = run([exe])
            check(ran.returncode == 0 and "counter 42" in ran.stdout, f"{backend}: reads and writes shared library data through a copy relocation", ran.stdout + ran.stderr)
            relocs = readelf("-r", exe)
            check("R_X86_64_COPY" in relocs or "R_X86_64_GLOB_DAT" in relocs, f"{backend}: the data symbol is resolved through a copy relocation or the GOT", relocs)

    exe = root / "no rpath"
    r = dcc(source, "-o", exe, "-flibdcext", "-fwhole-program", f"-L{libdir}", "-lshim")
    check(r.returncode == 0, "links without a runtime path", r.stderr)
    if r.returncode == 0:
        check(dynamic_entries(exe, "RUNPATH") == [], "no -rpath means no DT_RUNPATH")
        ran = run([exe], env={"LD_LIBRARY_PATH": ""})
        check(ran.returncode != 0 and "libshim.so" in ran.stderr, "a missing shared library at run time is reported by the loader", ran.stderr)
        ran = run([exe], env={"LD_LIBRARY_PATH": str(libdir)})
        check(ran.returncode == 0, "LD_LIBRARY_PATH finds the library at run time", ran.stderr)

    bundle = root / "bundle dir"
    (bundle / "lib").mkdir(parents=True)
    shutil.copy(library, bundle / "lib" / "libshim.so")
    exe = bundle / "app"
    r = dcc(source, "-o", exe, "-flibdcext", "-fwhole-program", f"-L{libdir}", "-lshim", "-rpath", "$ORIGIN/lib")
    check(r.returncode == 0, "links with an $ORIGIN runtime path", r.stderr)
    if r.returncode == 0:
        moved = root / "moved bundle"
        shutil.move(str(bundle), moved)
        ran = run([moved / "app"], env={"LD_LIBRARY_PATH": ""})
        check(ran.returncode == 0, "$ORIGIN finds the library next to a moved executable", ran.stderr)

    hello = root / "hello.dc"
    hello.write_text(HELLO_DC)
    for flags, what in ((["-dynamic"], "-dynamic"), (["-rpath", str(libdir)], "-rpath")):
        r = dcc(hello, "-o", root / "forced dynamic", "-flibdcext", "-fwhole-program", *flags)
        check(r.returncode != 0 and "needs a shared library to link" in r.stderr, f"{what} without a shared library is rejected instead of writing a broken executable", r.stderr)

    depfile = root / "call.d"
    exe = root / "with depfile"
    r = dcc(source, "-o", exe, "-flibdcext", "-fwhole-program", f"-L{libdir}", "-lshim", "--depfile", depfile)
    check(r.returncode == 0 and "libshim.so" in depfile.read_text(), "the depfile lists the linked shared library", r.stderr + (depfile.read_text() if depfile.exists() else ""))


def test_static_builds_stay_static(root):
    hello = root / "hello.dc"
    hello.write_text(HELLO_DC)
    for backend in ("llvm", "custom"):
        exe = root / f"static-{backend}"
        r = dcc(hello, "-o", exe, "-flibdcext", f"-fbackend={backend}", "-fwhole-program")
        check(r.returncode == 0, f"{backend}: a program without libraries links", r.stderr)
        if r.returncode == 0:
            check(interpreter(exe) is None, f"{backend}: a program without libraries has no PT_INTERP")
            check("DYNAMIC" not in readelf("-l", exe), f"{backend}: a program without libraries has no PT_DYNAMIC")
            check(run([exe]).returncode == 0, f"{backend}: the static program runs")
    archive_dir = root / "static libs"
    archive_dir.mkdir()
    obj = archive_dir / "shim.o"
    (archive_dir / "shim.c").write_text(SHIM_C)
    check(run([CC, "-c", "-fno-pic", "-o", obj, archive_dir / "shim.c"]).returncode == 0, "builds a static C archive member")
    check(run(["ar", "rcs", archive_dir / "libshim.a", obj]).returncode == 0, "builds a static C archive")
    source = root / "call-static.dc"
    source.write_text(CALL_DC)
    exe = root / "archive"
    r = dcc(source, "-o", exe, "-flibdcext", "-fwhole-program", f"-L{archive_dir}", "-lshim")
    check(r.returncode == 0 and interpreter(exe) is None, "a library found only as lib<name>.a keeps the executable static", r.stderr)
    if r.returncode == 0:
        check(run([exe]).returncode == 0, "the statically linked library works")
    both = root / "both libs"
    both.mkdir()
    shutil.copy(archive_dir / "libshim.a", both / "libshim.a")
    cbuild_shared(both, "shim", SHIM_C)
    exe = root / "forced static"
    r = dcc(source, "-o", exe, "-flibdcext", "-fwhole-program", "-static", f"-L{both}", "-lshim")
    check(r.returncode == 0 and interpreter(exe) is None, "-static picks lib<name>.a when lib<name>.so is also present", r.stderr)


def test_link_diagnostics(root):
    libdir = root / "diag libs"
    cbuild_shared(libdir, "shim", SHIM_C)
    source = root / "diag.dc"
    source.write_text(CALL_DC)
    r = dcc(source, "-o", root / "x1", "-flibdcext", "-fwhole-program", "-lnosuch_dcc_library")
    check(r.returncode != 0 and "unable to find library -lnosuch_dcc_library" in r.stderr and "library search path" in r.stderr,
          "a missing library names the searched directories", r.stderr)
    r = dcc(source, "-o", root / "x2", "-flibdcext", "-fwhole-program", "-static", f"-L{libdir}", "-lshim")
    check(r.returncode != 0 and "resolves only to the shared object" in r.stderr, "-static explains that only a shared object exists", r.stderr)
    r = dcc(source, "-o", root / "x3", "-flibdcext", "-fwhole-program", "-static", "-rpath", "/opt/x")
    check(r.returncode != 0 and "-rpath needs a dynamically linked executable" in r.stderr, "-rpath with -static is rejected", r.stderr)
    freestanding = root / "fs.dc"
    freestanding.write_text("module main;\n\n@nomangle extern i32 shim_add(i32 a, i32 b);\n\n@nomangle public void _start() {\n    shim_add(1, 2);\n}\n")
    for triple in ("x86-elf", "x86_64-elf"):
        r = dcc(freestanding, "-target", triple, "-o", root / f"fs-{triple}", f"-L{libdir}", "-lshim", "-dynamic")
        check(r.returncode != 0 and "dynamic linking is supported only for x86_64 Linux targets" in r.stderr, f"{triple}: dynamic linking is rejected", r.stderr)


def sdl3_available():
    for directory in ("/usr/local/lib", "/usr/lib", "/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"):
        if (Path(directory) / "libSDL3.so").exists():
            return True
    return False


def test_sdl3(root):
    if not sdl3_available():
        print("  INFO     SDL3 development library (libSDL3.so) not installed; the SDL3 check was not run")
        return
    source = root / "sdl.dc"
    source.write_text("""module main;

@nomangle extern i32 SDL_GetVersion();

public i32 main() {
    i32 version = SDL_GetVersion();
    println("SDL {}.{}.{}", version / 1000000, (version / 1000) % 1000, version % 1000);
    return if version / 1000000 == 3 { 0 } else { 1 };
}
""")
    for backend in ("llvm", "custom"):
        exe = root / f"sdl-{backend}"
        r = dcc(source, "-o", exe, "-flibdcext", f"-fbackend={backend}", "-fwhole-program", "-lSDL3")
        check(r.returncode == 0, f"{backend}: links SDL3 from the system library directories", r.stderr)
        if r.returncode == 0:
            ran = run([exe])
            check(ran.returncode == 0 and ran.stdout.startswith("SDL 3."), f"{backend}: SDL_GetVersion reports SDL 3", ran.stdout + ran.stderr)
            check(any(n.startswith("libSDL3.so") for n in dynamic_entries(exe, "NEEDED")), f"{backend}: DT_NEEDED names SDL3's soname")


def main():
    if CC is None or READELF is None:
        print("error: test_shared_link.py needs a host C compiler and readelf", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="dcc shared link ") as directory:
        root = Path(directory)
        test_custom_shared_library_output(root)
        test_consume_shared_library(root)
        test_static_builds_stay_static(root)
        test_link_diagnostics(root)
        test_sdl3(root)
    if failures:
        print(f"  FAIL     test_shared_link: {len(failures)} of {checks} checks failed")
        return 1
    print(f"  PASS     test_shared_link ({checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
