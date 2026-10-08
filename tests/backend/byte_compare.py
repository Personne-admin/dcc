#!/usr/bin/env python3

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, cwd, env=None, log=None, check=True):
    merged = dict(os.environ, LC_ALL="C", LANG="C.UTF-8", CXX="clang++", CC="clang", AR="ar")
    if env:
        merged.update(env)
    with log.open("ab") as output:
        result = subprocess.run(
            ["systemd-inhibit", "--what=sleep", "--why=dcc ci", *map(str, command)],
            cwd=cwd,
            env=merged,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
    if check and result.returncode:
        raise RuntimeError("command failed (%d): %s%s" % (result.returncode, " ".join(map(str, command)), " in " + str(log) if log else ""))
    return result.returncode


def compiler(tree, args, artifact, log):
    code = run(["timeout", "60", tree / "build/bin/dcc", "-ffile-prefix-map=" + str(tree) + "=dcc", *args], tree, log=log, check=False)
    if code == 0:
        status = "OK"
    elif code == 124:
        status = "TIMEOUT 124"
    elif code < 0 or code >= 128:
        status = "CRASH %d" % code
    else:
        status = "ERROR %d" % code
    artifact.with_name(artifact.name + ".status").write_text(status + "\n")
    if code:
        artifact.unlink(missing_ok=True)


def run_tasks(tree, tasks, log, jobs):
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for future in [pool.submit(compiler, tree, args, artifact, log) for args, artifact in tasks]:
            future.result()


def capture_abi(tree, output, log, jobs):
    spec = importlib.util.spec_from_file_location("cross_backend", tree / "tests/abi/cross_backend.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    cases = [c for c in module.build_cases() if not module.is_based(c.ret) and not any(module.is_based(t) for t in c.args)]
    source = output / "abi-source"
    source.mkdir()
    (source / "abi_caller.dc").write_text(module.gen_dc_caller(cases))
    (source / "abi_callee.dc").write_text(module.gen_dc_callee(cases))
    tasks = []
    for target in ("x86_64-elf", "x86_64-coff"):
        for opt in ("O0", "O2"):
            for side in ("caller", "callee"):
                stem = "%s-%s-%s" % (target, opt, side)
                for kind, flag, suffix in (("object", "-c", ".o"), ("asm", "-S", ".s")):
                    artifact = output / "abi" / kind / (stem + suffix)
                    artifact.parent.mkdir(parents=True, exist_ok=True)
                    tasks.append((["-target", target, "-fbackend", "custom", "-" + opt, "-I", str(source), "-ffile-prefix-map=" + str(source) + "=dcc-abi", flag, "-o", str(artifact), str(source / ("abi_%s.dc" % side))], artifact))
    run_tasks(tree, tasks, log, jobs)
    shutil.rmtree(source)


def capture_libdcext(tree, output, log, jobs):
    sources = sorted((tree / "libdcext/std").rglob("*.dc")) + [tree / "libdcext/assert.dc"]
    tasks = []
    for target, os_name in (("x86_64-elf", "linux"), ("x86_64-coff", "windows")):
        for opt in ("O0", "O2"):
            for source in sources:
                relative = source.relative_to(tree / "libdcext")
                for kind, flag, suffix in (("object", "-c", ".o"), ("asm", "-S", ".s")):
                    artifact = output / "libdcext" / target / opt / kind / (str(relative) + suffix)
                    artifact.parent.mkdir(parents=True, exist_ok=True)
                    tasks.append((["-flibdcext", os_name, "-target", target, "-fbackend", "custom", "-" + opt, "-I", str(tree / "libdcext"), flag, "-o", str(artifact), str(source)], artifact))
    run_tasks(tree, tasks, log, jobs)


def make_manifest(output, revision):
    files = {}
    for path in sorted(output.rglob("*")):
        if path.is_file() and path.name not in ("manifest.json", "capture.log"):
            relative = str(path.relative_to(output))
            files[relative] = {"bytes": path.stat().st_size, "sha256": digest(path)}
    manifest = {"revision": revision, "files": files}
    (output / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")
    return manifest


def capture(tree, output, jobs):
    if output.exists():
        raise RuntimeError("capture directory already exists: %s" % output)
    output.mkdir(parents=True)
    log = output / "capture.log"
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=tree, text=True).strip()
    run(["make", "-j%d" % jobs, "all", "DCC_WRAPPER=timeout 60"], tree, log=log)
    dispatcher = tree / "build/bin/tests/test_cases_dispatcher"
    run(["make", "-j%d" % jobs, "byte-capture-dispatcher", "DCC_WRAPPER=timeout 60"], tree, log=log)
    run([dispatcher], tree / "tests", {"DCC_BYTE_CAPTURE_ROOT": str(output), "DCC_BYTE_CAPTURE_ONLY": "1", "DCC_BYTE_JOBS": str(jobs), "DCC_BYTE_SOURCE_ROOT": str(tree), "DCC_TEST_LIBDCEXT_SRC": str(tree / "libdcext"), "DCC_TEST_LIBDCEXT_A": str(tree / "build/lib/libdcext-linux-custom.a")}, log)
    capture_libdcext(tree, output, log, jobs)
    capture_abi(tree, output, log, jobs)
    manifest = make_manifest(output, revision)
    files = manifest["files"]
    counts = {name: sum(path.startswith(name + "/") for path in files) for name in ("fixtures", "libdcext", "abi")}
    print("capture %s: %d files, %d bytes, %s" % (revision[:12], len(files), sum(entry["bytes"] for entry in files.values()), counts))


def compare(baseline, candidate):
    left = json.loads((baseline / "manifest.json").read_text())["files"]
    right = json.loads((candidate / "manifest.json").read_text())["files"]
    for name in sorted(set(left) | set(right)):
        if left.get(name) != right.get(name):
            raise RuntimeError("byte mismatch: %s baseline=%s candidate=%s" % (name, left.get(name), right.get(name)))
    print("byte-identical: %d files, %d bytes" % (len(left), sum(item["bytes"] for item in left.values())))


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    capture_parser = commands.add_parser("capture")
    capture_parser.add_argument("--tree", type=Path, required=True)
    capture_parser.add_argument("--out", type=Path, required=True)
    compare_parser = commands.add_parser("compare")
    compare_parser.add_argument("--tree", type=Path, required=True)
    compare_parser.add_argument("--baseline", type=Path, required=True)
    for sub in (capture_parser, compare_parser):
        sub.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    try:
        tree = args.tree.resolve()
        if args.command == "capture":
            capture(tree, args.out.resolve(), args.jobs)
        else:
            baseline = args.baseline.resolve()
            with tempfile.TemporaryDirectory(prefix="dcc-byte-compare-", dir=baseline.parent) as temporary:
                candidate = Path(temporary) / "capture"
                capture(tree, candidate, args.jobs)
                compare(baseline, candidate)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
