#!/usr/bin/env python3
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

CHECK_FLAGS = {"-fbounds-check", "-frestricted-check"}
ASSERT_EXIT_CODE = 0xA5
DEFAULT_CONFIGS = (("em64t", "O0"), ("em64t", "O1"), ("em64t", "O2"), ("llvm", "O0"))


def repository_root():
    return Path(__file__).resolve().parents[2]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_hashes(root, backends):
    targets = {"compiler": root / "build/bin/dcc"}
    for backend in backends:
        targets["lib-" + backend] = root / ("build/lib/libdcext-linux-" + backend + ".a")
    return {name: sha256(path) for name, path in targets.items()}


def run(command, cwd, timeout):
    try:
        done = subprocess.run([str(part) for part in command], capture_output=True, timeout=timeout, cwd=cwd)
        return done.returncode, done.stdout, done.stderr
    except subprocess.TimeoutExpired:
        return 124, b"", b"timeout"


def decode_status(code):
    if code < 0:
        return "signal:" + str(-code)
    return "exit:" + str(code)


def program_flags(path):
    flags = []
    with open(path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            stripped = line.strip()
            if stripped.startswith("// DCCFLAGS:"):
                flags += stripped[len("// DCCFLAGS:"):].split()
    return flags


def ensure_deps(root, work):
    runtime = work / "runtime.o"
    if not runtime.exists():
        code, _, err = run(["clang", "-O2", "-ffreestanding", "-fno-builtin", "-c", root / "libdcext/benchmarks/runtime.c", "-o", runtime], cwd=work, timeout=180)
        if code:
            raise SystemExit("failed to build runtime.o: " + err.decode(errors="replace"))

    stub_lines = [
        ".text",
        ".globl _DC0F1.6.assert8.__assert4SCqci32sSCqcSCqcv",
        ".type _DC0F1.6.assert8.__assert4SCqci32sSCqcSCqcv, @function",
        "_DC0F1.6.assert8.__assert4SCqci32sSCqcSCqcv:",
        "    mov $" + str(ASSERT_EXIT_CODE) + ", %edi",
        "    mov $60, %eax",
        "    syscall",
        ".size _DC0F1.6.assert8.__assert4SCqci32sSCqcSCqcv, . - _DC0F1.6.assert8.__assert4SCqci32sSCqcSCqcv",
        "",
    ]
    stub_source = work / "assert_exit.s"
    stub_object = work / "assert_exit.o"
    stub_source.write_text(chr(10).join(stub_lines))
    code, _, err = run(["clang", "-c", stub_source, "-o", stub_object], cwd=work, timeout=180)
    if code:
        raise SystemExit("failed to assemble assert stub: " + err.decode(errors="replace"))
    return runtime, stub_object


def compile_and_run(compiler, program, backend, opt, extra, work, root, runtime, stub):
    exe = work / (program.stem + "." + backend + "." + opt)
    base = [compiler, "-flibdcext", "linux", "-target", "x86_64-elf", "-fbackend=" + backend, "-" + opt]

    if any(flag in CHECK_FLAGS for flag in extra):
        obj = work / (program.stem + "." + backend + "." + opt + ".o")
        code, out, err = run([*base, *extra, "-c", "-o", obj, program], cwd=work, timeout=300)
        if code:
            return ("compile-fail", code, out, err)

        lib = root / ("build/lib/libdcext-linux-" + backend + ".a")
        code, out, err = run(["ld.lld", "-e", "_start", "-u", "_start", obj, stub, runtime, lib, "-o", exe],
                             cwd=work, timeout=300)
        if code:
            return ("link-fail", code, out, err)

        code, out, err = run([exe], cwd=work, timeout=180)
        return ("run", code, out, err)

    code, out, err = run([*base, *extra, "-o", exe, program], cwd=work, timeout=300)
    if code:
        return ("compile-fail", code, out, err)

    code, out, err = run([exe], cwd=work, timeout=180)
    return ("run", code, out, err)


def expand_inputs(inputs):
    programs = []
    for item in inputs:
        path = Path(item).resolve()
        if path.is_dir():
            programs += sorted(path.rglob("*.dc"))
        else:
            programs.append(path)

    if not programs:
        raise SystemExit("no programs found in: " + " ".join(str(i) for i in inputs))

    return programs


def parse_configs(values):
    configs = []
    for value in values:
        backend, _, opt = value.partition(":")
        configs.append((backend, opt))
    return configs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+", help=".dc program files or directories of .dc programs")
    parser.add_argument("--compiler", default=None, help="dcc binary")
    parser.add_argument("--configs", nargs="+", default=[backend + ":" + opt for backend, opt in DEFAULT_CONFIGS], help="backend configs as backend:opt")
    parser.add_argument("--work", default=None, help="scratch directory")
    parser.add_argument("--baseline", default=None, help="JSON file of expected artifact hashes")
    parser.add_argument("--record-baseline", default=None, help="write current artifact hashes to this JSON file")
    parser.add_argument("--allow-broken", action="store_true", help="do not fail the run when programs fail to build on every config")
    args = parser.parse_args()

    root = repository_root()
    compiler = Path(args.compiler).resolve() if args.compiler else root / "build/bin/dcc"
    work = Path(args.work).resolve() if args.work else root / "build/diffrun-work"
    work.mkdir(parents=True, exist_ok=True)
    configs = parse_configs(args.configs)
    backends = sorted({backend for backend, _ in configs})
    runtime, stub = ensure_deps(root, work)

    start = artifact_hashes(root, backends)
    if args.record_baseline:
        Path(args.record_baseline).write_text(json.dumps(start, indent=2) + chr(10))
        print("baseline written to " + args.record_baseline)

    if args.baseline:
        expected = json.loads(Path(args.baseline).read_text())
        if start != expected:
            print("HASH MISMATCH")
            for key in sorted(expected):
                if start.get(key) != expected.get(key):
                    print("  " + key + chr(10) + "    baseline: " + str(expected.get(key)) + chr(10) + "    actual: " + str(start.get(key)))
            return 2

    print("hashes recorded for this run")

    ran = 0
    broken = 0
    diverged = 0
    for program in expand_inputs(args.inputs):
        extra = program_flags(program)
        fingerprints = {}
        for backend, opt in configs:
            kind, code, out, err = compile_and_run(compiler, program, backend, opt, extra, work, root, runtime, stub)
            fingerprints[(backend, opt)] = (kind, decode_status(code), out, err)
        if len(set(fingerprints.values())) == 1:
            kind, status, out, err = next(iter(fingerprints.values()))
            suffix = " flags=" + " ".join(extra) if extra else ""
            if kind == "run":
                ran += 1
                print(program.name + ": OK (" + status + ", stdout=" + str(len(out)) + "B)" + suffix)
            else:
                broken += 1
                print(program.name + ": BROKEN (" + kind + " " + status + " on every config)" + suffix)
                print("  " + repr(err[:400]))
        else:
            diverged += 1
            print(program.name + ": DIVERGE")
            for key in configs:
                kind, status, out, err = fingerprints[key]
                print("  " + key[0] + ":" + key[1] + " " + kind + " " + status + " stdout=" + repr(out[:160]) + " stderr=" + repr(err[:200]))

    total = ran + broken + diverged
    print("RESULT: total=" + str(total) + " ran=" + str(ran) + " broken=" + str(broken) + " diverged=" + str(diverged))

    end = artifact_hashes(root, backends)
    if end != start:
        print("HASH CHANGED DURING RUN")
        return 1
    if diverged:
        return 1
    if broken and not args.allow_broken:
        return 1

    print("ALL-OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
