from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="dcc-i8086-models-") as directory:
    source = Path(directory) / "main.dc"
    source.write_text("module m;\nvoid f() {}\n")
    checks = 0
    for target, model, arch, message in (
        ("i8086-binary", "unreal", "8086", "-mcmodel=unreal requires -farch i386 or newer; got -farch 8086"),
        ("i8086-binary", "unreal32", "i286", "-mcmodel=unreal32 requires -farch i386 or newer; got -farch i286"),
        ("i8086-binary", "large", "i386", "-mcmodel=large is not supported for target 'i8086-binary'"),
        ("x86_64-elf", "unreal", "generic", "-mcmodel=unreal is not supported for target 'x86_64-elf'"),
        ("x86-elf", "unreal32", "i386", "-mcmodel=unreal32 is not supported for target 'x86-elf'"),
    ):
        result = subprocess.run(["timeout", "60", str(compiler), "-target", target, "-mcmodel", model,
                                 "-farch", arch, "-c", str(source), "-o", str(Path(directory) / "out.o")], capture_output=True, text=True)
        assert result.returncode == 1 and message in result.stderr, result.stderr
        checks += 1
    for model in ("default", "small", "unreal", "unreal32"):
        result = subprocess.run(["timeout", "60", str(compiler), "-target", "i8086-binary", "-mcmodel", model,
                                 "-fbackend", "custom", "-c", str(source), "-o", str(Path(directory) / "out.o")], capture_output=True, text=True)
        assert result.returncode == 1 and "no backend for target 'i8086-binary'" in result.stderr, result.stderr
        checks += 1
    print("  RESULT  i8086 model diagnostics: %d/%d passed" % (checks, checks))
