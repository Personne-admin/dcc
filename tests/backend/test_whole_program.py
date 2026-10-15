from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
runner = Path(__file__).resolve().parent.parent / "i8086" / "run.py"
checks = 0

MODULES = {
    "main.dc": "module app;\nimport mid;\nusing mid::*;\n@nomangle public i32 dcc_main() {\n    i32 v = compute(4);\n    if v != 17 { return 1; }\n    if bias != 5 { return 2; }\n    return 0;\n}\n",
    "mid.dc": "module mid;\nimport leaf;\nimport box;\npublic i32 bias = 5;\npublic i32 compute(i32 x) {\n    box::Box(i32) b = {value = x};\n    return leaf::double_it(box::unwrap(b)) + bias + 4;\n}\n",
    "leaf.dc": "module leaf;\npublic i32 double_it(i32 x) { return x * 2; }\npublic i32 unused_helper(i32 x) { return x + 100; }\n",
    "box.dc": "module box;\npublic struct Box(T) { T value; }\npublic T unwrap(T)(Box(T) b) { return b.value; }\n",
}

MAIN_C = "int dcc_main(void);\nint main(void) { return dcc_main(); }\n"

I8086_CRT0 = "bits 16\nsection .start progbits alloc exec\nextern dcc_main\n\tcall dcc_main\n\tmov dx, 0xf4\n\tout dx, al\n.hang:\n\tjmp .hang\n"


def run(*args, expect=0, contains=None):
    global checks
    result = subprocess.run(["timeout", "60", *map(str, args)], capture_output=True, text=True)
    assert result.returncode == expect, (args, result.returncode, result.stdout, result.stderr)
    if contains is not None:
        assert contains in result.stderr, (args, result.stderr)
    checks += 1
    return result


def write_sources(directory):
    for name, text in MODULES.items():
        (directory / name).write_text(text)
    (directory / "main.c").write_text(MAIN_C)
    return directory / "main.dc"


with tempfile.TemporaryDirectory(prefix="dcc-whole-program-") as directory:
    directory = Path(directory)
    entry = write_sources(directory)
    cc = shutil.which("cc") or shutil.which("clang")
    assert cc, "need a C compiler for the standalone object check"

    i8086 = [compiler, "-target", "i8086-binary", "-fbackend", "custom"]
    asm = directory / "whole.asm"
    run(*i8086, "-fwhole-program", "-S", entry, "-o", asm)
    text = asm.read_text()
    assert "extern _DC" not in text, "imported functions or globals left as extern"
    assert "unused_helper" not in text, "unreferenced imported function was emitted"
    assert "_DC0F1.3.mid7.compute" in text and "_DC0F1.4.leaf9.double_it" in text and "_DC0G1.3.mid4.bias" in text, text
    startup = directory / "crt0.asm"
    startup.write_text(I8086_CRT0)
    run("nasm", "-f", "elf32", startup, "-o", directory / "crt0.o")
    run("nasm", "-f", "elf32", asm, "-o", directory / "whole.o")
    binary = directory / "whole.bin"
    run(*i8086, "-mcmodel=small", "-fbase=1000:0", directory / "crt0.o", directory / "whole.o", "-o", binary)
    run(sys.executable, runner, "--load", "1000:0000", "--mode", "real", binary, expect=0)

    for backend in ("custom", "llvm"):
        obj = directory / f"whole-{backend}.o"
        run(compiler, "-fbackend", backend, "-fwhole-program", "-c", entry, "-o", obj)
        exe = directory / f"whole-{backend}"
        run(cc, "-o", exe, directory / "main.c", obj)
        run(exe, expect=0)

    plain = directory / "plain.o"
    run(compiler, "-fbackend", "custom", "-c", entry, "-o", plain)
    run(cc, "-o", directory / "plain", directory / "main.c", plain, expect=1)

    run(compiler, "-flibdcext", "-fwhole-program", entry, "-o", directory / "linked", expect=1,
        contains="-fwhole-program with -flibdcext cannot link an executable or shared library")

    reach = directory / "reach.dc"
    reach.write_text("module app;\n\n@nomangle public void loader_entry() {\n    reachable();\n}\n\nvoid unreachable_creator() {\n    u8[2] a = {0, 0};\n    [] u8 s = a;\n    s[5] = 1;\n}\n\nvoid reachable() {\n    u8[2] b = {0, 0};\n    [] u8 t = b;\n    t[6] = 2;\n}\n")
    i8086_reach = directory / "reach-i8086.s"
    run(*i8086, "-mcmodel", "unreal", "-O2", "-fwhole-program", "-fbounds-check", "-fpartial-eval", "-e", "loader_entry", "-S", reach, "-o", i8086_reach)
    i8086_text = i8086_reach.read_text()
    assert "__assert" in i8086_text, "i8086 whole-program dropped __assert:\n" + i8086_text
    em64t_reach = directory / "reach-em64t.s"
    run(compiler, "-target", "x86_64-elf", "-O2", "-fbackend", "custom", "-fwhole-program", "-fbounds-check", "-fpartial-eval", "-e", "loader_entry", "-S",
        reach, "-o", em64t_reach)
    em64t_text = em64t_reach.read_text()
    assert "__assert" in em64t_text, "em64t whole-program dropped __assert:\n" + em64t_text

    entry_shape = directory / "entry.dc"
    entry_shape.write_text("module app;\n\n@nomangle public void loader_entry() {\n    used();\n}\n\npublic void unused_public() {\n    [] const char s = \"unused-public-literal\";\n    used();\n}\n\n@nomangle public void unused_nomangle() {\n    used();\n}\n\nvoid dropped_literal_user() {\n    [] const char s = \"dropped-literal\";\n    used();\n}\n\nvoid used() {\n}\n")
    with_e = directory / "entry-with-e.s"
    result = run(*i8086, "-mcmodel", "unreal", "-O2", "-fwhole-program", "-e", "loader_entry", "--print-reachability", "-S", entry_shape, "-o", with_e)
    report = result.stdout
    text = with_e.read_text()
    assert "unused_nomangle" in text, "with -e the @nomangle definition must stay a root:\n" + text
    assert "unused_public" not in text, "with -e an unused public entry-module function must be dropped:\n" + text
    assert "dropped_literal_user" not in text, "with -e a dropped function must not be emitted:\n" + text
    assert "reachability: dropped function" in report, report
    assert "reachability: dropped global" in report, report
    without_e = directory / "entry-without-e.s"
    run(*i8086, "-mcmodel", "unreal", "-O2", "-fwhole-program", "-S", entry_shape, "-o", without_e)
    assert "unused_public" in without_e.read_text(), "without -e a public entry-module function is a root"

    assert_dir = directory / "assert_project"
    assert_dir.mkdir()
    (assert_dir / "app.dc").write_text("module app;\n\n@nomangle public void loader_entry() {\n    u8[2] a = {0, 0};\n    [] u8 s = a;\n    s[5] = 1;\n}\n")
    (assert_dir / "assert.dc").write_text("module assert;\n\npublic import core::source_location;\n\npublic void __assert([] const char msg, core::source_location::SourceLocation sl) {\n    for ;; {}\n}\n")
    implicit = directory / "implicit-assert.s"
    result = run(*i8086, "-mcmodel", "unreal", "-O2", "-fwhole-program", "-fbounds-check", "-e", "loader_entry", "--print-reachability", "-S",
                 assert_dir / "app.dc", "-o", implicit)
    assert "6.assert8.__assert" in result.stdout, "assert module must always be imported and __assert kept:\n" + result.stdout
    assert "6.assert8.__assert" in implicit.read_text(), "assert definition must be emitted:\n" + implicit.read_text()

print(f"PASS {checks} whole-program checks")
