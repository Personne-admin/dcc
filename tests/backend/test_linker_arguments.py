from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    dcc = Path(sys.argv[1]).resolve()
    real_linker = shutil.which("ld.lld")
    with tempfile.TemporaryDirectory(prefix="dcc-linker-args-") as temp:
        root = Path(temp)
        (root / "bin").mkdir()
        log = root / "linker.json"
        wrapper = root / "bin/ld.lld"
        wrapper.write_text("#!/usr/bin/env python3\nimport json, os, sys\nfrom pathlib import Path\n"
                           + "Path(" + repr(str(log)) + ").write_text(json.dumps(sys.argv[1:]))\n"
                           + "os.execv(" + repr(real_linker) + ", [" + repr(real_linker) + ", *sys.argv[1:]])\n")
        wrapper.chmod(0o755)
        env = dict(os.environ, PATH=str(root / "bin") + os.pathsep + os.environ["PATH"])

        def run(args):
            result = subprocess.run(["timeout", "60", *map(str, args)], env=env, capture_output=True, text=True, timeout=65)
            assert result.returncode == 0, (args, result.stdout, result.stderr)
            return result.stdout

        source = root / "start.dc"
        source.write_text("module start; @nomangle public void _start() {}\n")
        cases = (
            ("-Wl,-static", ["-static"]),
            ("-Wl,-z,noexecstack", ["-z", "noexecstack"]),
            ("-Wl,-z,max-page-size=0x1000,--no-pie", ["-z", "max-page-size=0x1000", "--no-pie"]),
            ("-Wl,--defsym=answer=42", ["--defsym=answer=42"]),
        )
        for backend in ("llvm", "custom"):
            obj = root / "start.o"
            run([dcc, "-c", "-fbackend", backend, source, "-o", obj])
            for input_file in (source, obj):
                for option, expected in cases:
                    run([dcc, "-fbackend", backend, "-e", "_start", option, input_file, "-o", root / "out"])
                    args = json.loads(log.read_text())
                    assert any(args[i:i + len(expected)] == expected for i in range(len(args))), (option, args)
                    assert not any(arg.startswith(",") for arg in args), args
                    if "answer=42" in option:
                        assert re.search(r"000000000000002a\s+A\s+answer", run(["nm", root / "out"])), args
            run([dcc, "-c", "-flibdcext", "freestanding", "-fno-simd", "-fno-x87", "-fno-red-zone", "-fpic", "-fbackend", backend, source, "-o", obj])
            script = root / "script.lds"
            script.write_text("ENTRY(_start)\nSECTIONS { . = 0x100000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }\n")
            output = root / "kernel"
            run([dcc, "-o", output, obj, "-target", "x86_64-elf", "-fbackend", backend,
                 "-flibdcext", "freestanding", "-T", script, "--gc-sections", "-Wl,-static", "-Wl,--no-pie",
                 "-Wl,-z,max-page-size=0x1000", "-Wl,-z,noexecstack"])
            assert re.search(r"Type:\s+EXEC", run(["readelf", "-h", output]))
            headers = run(["readelf", "-lW", output])
            assert "INTERP" not in headers and "DYNAMIC" not in headers, headers
            loads = [line for line in headers.splitlines() if line.strip().startswith("LOAD")]
            assert loads and all(line.split()[-1] == "0x1000" for line in loads), headers
            stack = next(line for line in headers.splitlines() if "GNU_STACK" in line)
            assert "E" not in stack.split()[-2], stack
            assert "There is no dynamic section" in run(["readelf", "-d", output])
            symbols = run(["nm", output])
            assert "__dcc_libdcext_abi_requires_float_disabled" in symbols, symbols
            for forbidden in ("dcc_main", "__libc_start_main", "_GLOBAL_OFFSET_TABLE_", "__libc_csu_init"):
                assert forbidden not in symbols, symbols
    print("PASS linker comma splitting on both backends, source/object links, and static freestanding EXEC without hosted startup")


if __name__ == "__main__":
    main()
