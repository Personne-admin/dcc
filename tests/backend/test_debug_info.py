from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
top = Path(__file__).resolve().parent.parent.parent
libdcext = top / "libdcext"
environment = dict(os.environ, DCC_STRICT_DEBUG_LOCATIONS="1")

targets = {
    "linux": ["-target", "x86_64-elf"],
    "windows": ["-target", "x86_64-coff"],
    "freestanding": ["-target", "x86_64-elf", "-fno-simd", "-fno-x87", "-fno-red-zone", "-fpic"],
}
freestanding_excluded = {"std/sys/linux/syscall.dc", "std/sys/win/imports.dc"}
library_sources = sorted((libdcext / "std").rglob("*.dc")) + [libdcext / "assert.dc", libdcext / "common" / "abi.dc"]

regression = """module m;
import std::result;
using std::result::Result;
Result(i32, i32) make(i32 x) {
    return Result(i32, i32)::Err(x);
}
@nomangle public i32 entry(i32 x) {
    return make(x).unwrap_err();
}
"""


def program_os(path):
    if path.stem.endswith("-windows") or path.stem == "win64-stack-probe":
        return "windows"
    return "linux"


def compile_one(job):
    source, os_name, backend, level = job
    command = [str(compiler), *targets[os_name], "-g", level, "-fbackend", backend, "-flibdcext", os_name, f"-I{libdcext}", "-c", str(source), "-o", os.devnull]
    result = subprocess.run(["timeout", "60", *command], capture_output=True, text=True, env=environment)
    return command, result


with tempfile.TemporaryDirectory(prefix="dcc-debug-info-") as directory:
    regression_source = Path(directory) / "unwrap_err.dc"
    regression_source.write_text(regression)

    jobs = []
    for backend in ("llvm", "custom"):
        for level in ("-O0", "-O2"):
            for os_name in targets:
                for source in library_sources:
                    relative = source.relative_to(libdcext).as_posix()
                    if os_name == "freestanding" and relative in freestanding_excluded:
                        continue
                    jobs.append((source, os_name, backend, level))
            for source in sorted((top / "tests" / "stdlib").glob("*.dc")):
                jobs.append((source, program_os(source), backend, level))
            for os_name in ("linux", "freestanding"):
                jobs.append((regression_source, os_name, backend, level))

    failures = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for command, result in pool.map(compile_one, jobs):
            if result.returncode != 0:
                failures += 1
                print(f"    FAIL  {' '.join(command)}", file=sys.stderr)
                for line in result.stderr.splitlines()[:8]:
                    print(f"          | {line}", file=sys.stderr)

    if failures:
        print(f"  RESULT  {failures}/{len(jobs)} debug-info compiles failed", file=sys.stderr)
        sys.exit(1)
    print(f"  RESULT  {len(jobs)}/{len(jobs)} debug-info compiles passed")
