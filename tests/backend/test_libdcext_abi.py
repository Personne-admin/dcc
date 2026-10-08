from pathlib import Path
import subprocess
import sys
import tempfile

ENABLED = "__dcc_libdcext_abi_requires_float_enabled"
DISABLED = "__dcc_libdcext_abi_requires_float_disabled"


def main():
    dcc = Path(sys.argv[1]).resolve()
    lib = dcc.parent.parent / "lib"
    with tempfile.TemporaryDirectory(prefix="dcc-abi-mode-") as temp:
        root = Path(temp)
        source = root / "start.dc"
        source.write_text("module start; @nomangle public void _start() {}\n")

        def invoke(args, success=True):
            p = subprocess.run(["timeout", "60", *map(str, args)], capture_output=True, text=True, timeout=65)
            assert (p.returncode == 0) == success, (args, p.stdout, p.stderr)
            return p.stdout + p.stderr

        for backend in ("llvm", "custom"):
            for opt in ("O0", "O2"):
                objects = {}
                for mode, marker, flags, os_name in (
                    (True, ENABLED, [], "linux"),
                    (False, DISABLED, ["-fno-simd", "-fno-x87"], "freestanding"),
                ):
                    obj = root / f"{backend}-{opt}-{mode}.o"
                    objects[mode] = obj
                    archive = lib / f"libdcext-{os_name}-{backend}.a"
                    compile_args = [dcc, "-flibdcext", os_name, "-fbackend", backend, "-" + opt, *flags]
                    invoke([*compile_args, "-c", source, "-o", obj])
                    assert marker in invoke(["nm", "-u", obj])
                    assert marker in invoke(["nm", "-g", "--defined-only", archive])
                    sections = invoke(["readelf", "-SW", obj])
                    guard = next(line for line in sections.splitlines() if ".data.rel.ro.dcc.libdcext.abi" in line)
                    assert any(flag in guard.split()[-4] for flag in ("R", "o")), guard
                    for gc in ([], ["--gc-sections"]):
                        invoke(["ld.lld", *gc, obj, archive, "-o", root / "raw"])
                        invoke([dcc, "-flibdcext", os_name, "-fbackend", backend, *gc, obj, "-o", root / "driver"])
                    invoke([*compile_args, "-e", "_start", source, "-o", root / "source"])
                    wrong_os = "freestanding" if mode else "linux"
                    wrong_archive = lib / f"libdcext-{wrong_os}-{backend}.a"
                    for gc in ([], ["--gc-sections"]):
                        error = invoke(["ld.lld", *gc, obj, wrong_archive, "-o", root / "bad"], False)
                        assert "undefined symbol" in error and marker in error, error
                        error = invoke([dcc, "-flibdcext", wrong_os, "-fbackend", backend, *gc, obj, "-o", root / "bad"], False)
                        assert "floating-point ABI mismatch" in error and marker in error and "-fno-simd" in error, error
                    error = invoke([dcc, "-flibdcext", wrong_os, "-fbackend", backend, *flags, "-e", "_start", source, "-o", root / "bad"], False)
                    assert "floating-point ABI mismatch" in error and marker in error, error
                dead = root / "dead.dc"
                dead.write_text("module dead; public i32 unused() { return 1; }\n")
                invoke([dcc, "-c", "-flibdcext", "linux", "-fbackend", backend, "-O2", dead, "-o", root / "dead.o"])
                error = invoke(["ld.lld", "--gc-sections", objects[False], root / "dead.o", lib / f"libdcext-freestanding-{backend}.a", "-o", root / "bad"], False)
                assert ENABLED in error, error
            coff_source = root / "coff.dc"
            coff_source.write_text("module std::prelude; @nomangle public void _start() {}\n")
            coff_archives = {}
            for mode in (True, False):
                flags = [] if mode else ["-fno-simd", "-fno-x87"]
                marker_object = root / f"marker-{mode}.obj"
                archive = root / f"marker-{mode}.a"
                invoke([dcc, "-c", "-target", "x86_64-coff", "-fbackend", backend, *flags,
                        Path(__file__).resolve().parents[2] / "libdcext/common/abi.dc", "-o", marker_object])
                invoke(["ar", "rcs", archive, marker_object])
                coff_archives[mode] = archive
            for opt in ("-O0", "-O2"):
                for mode, marker, flags in ((True, ENABLED, []), (False, DISABLED, ["-fno-simd", "-fno-x87"])):
                    coff = root / "coff.obj"
                    invoke([dcc, "-c", "-flibdcext", "windows", "-target", "x86_64-coff", "-fbackend", backend, opt, *flags, coff_source, "-o", coff])
                    assert ("/include:" + marker).encode() in coff.read_bytes()
                    link = ["lld-link", "/entry:_start", "/subsystem:native", "/nodefaultlib", "/opt:ref", "/out:" + str(root / "coff.exe"), coff]
                    invoke([*link, coff_archives[mode]])
                    assert marker in invoke([*link, coff_archives[not mode]], False)
        tags = root / "tags.dc"
        tags.write_text("""module tags;
import std::json;
import std::toml;
import core::target;
@nomangle public i32 tag_probe() {
    std::json::Event a = std::json::Event::Bool(true);
    std::json::Value b = std::json::Value::String(\"\");
    std::toml::Scalar c = std::toml::Scalar::Bool(true);
    std::toml::Value d = std::toml::Value::Bool(true);
    static if core::target::FLOAT_AVAILABLE {
        return (*(&a as u8*) != 8 || *(&b as u8*) != 4 || *(&c as u8*) != 3 || *(&d as u8*) != 3) as i32;
    } else {
        return (*(&a as u8*) != 7 || *(&b as u8*) != 3 || *(&c as u8*) != 2 || *(&d as u8*) != 2) as i32;
    }
}
""")
        harness = root / "harness.s"
        harness.write_text(".text\n.globl _start\n_start:\ncall tag_probe\nmov %eax,%edi\nmov $60,%eax\nsyscall\n.section .note.GNU-stack,\"\",@progbits\n")
        invoke(["clang", "-c", harness, "-o", root / "harness.o"])
        for backend in ("llvm", "custom"):
            for enabled in (True, False):
                os_name = "linux" if enabled else "freestanding"
                flags = [] if enabled else ["-fno-simd", "-fno-x87"]
                invoke([dcc, "-c", "-flibdcext", os_name, "-fbackend", backend, *flags, tags, "-o", root / "tags.o"])
                invoke(["ld.lld", root / "harness.o", root / "tags.o", lib / f"libdcext-{os_name}-{backend}.a", "-o", root / "tags"])
                result = subprocess.run([str(root / "tags")], timeout=60)
                assert result.returncode == 0, result.returncode
    print("PASS libdcext ABI markers, matched/mismatched driver/raw links, GC retention, COFF directives, and enum tag shifts")


if __name__ == "__main__":
    main()
