#!/usr/bin/env python3

import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Optional

SCRIPT_DIR = Path(__file__).resolve().parent
CORPUS_DIR = SCRIPT_DIR / "corpus"
META_PATH = CORPUS_DIR / "meta.json"
START_OBJECT = SCRIPT_DIR / "start.o"

DCC = Path("/usr/local/bin/dcc")
LINKER = "ld.lld"

BACKENDS: dict[str, list[str]] = {
    "em": ["-fbackend", "em64t"],
    "ll": [],
}


def new_result() -> dict[str, int]:
    return {
        "n": 0,
        "em_ok": 0,
        "llvm_ok": 0,
        "agree": 0,
    }


def run_command(command: list[str]) -> bool:
    result = subprocess.run(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def compile_and_run(
    source: Path,
    backend: str,
) -> Optional[int]:
    object_path = SCRIPT_DIR / f"s_{backend}.o"
    program_path = SCRIPT_DIR / f"s_{backend}"

    object_path.unlink(missing_ok=True)
    program_path.unlink(missing_ok=True)

    compile_command = [
        str(DCC),
        *BACKENDS[backend],
        "-target",
        "x86_64-elf",
        "-c",
        "-o",
        str(object_path),
        str(source),
    ]

    if not run_command(compile_command):
        return None

    link_command = [
        LINKER,
        "--static",
        "--no-dynamic-linker",
        "-e",
        "_start",
        "-o",
        str(program_path),
        str(object_path),
        str(START_OBJECT),
    ]

    if not run_command(link_command):
        return None

    result = subprocess.run(
        [str(program_path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode


def load_metadata() -> dict[str, dict[str, object]]:
    with META_PATH.open(encoding="utf-8") as file:
        metadata = json.load(file)

    if not isinstance(metadata, dict):
        raise ValueError("meta.json must contain a JSON object")

    return metadata


def print_result(label: str, result: dict[str, int]) -> None:
    print(
        f"{label:14s} "
        f"n={result['n']:3d}  "
        f"em64t_correct={result['em_ok']:3d}  "
        f"llvm_correct={result['llvm_ok']:3d}  "
        f"agree={result['agree']:3d}"
    )


def print_failures(
    title: str,
    failures: list[tuple[str, str, int, Optional[int]]],
    limit: int,
) -> None:
    if not failures:
        return

    print(f"\n{title}:")

    for name, category, expected, actual in failures[:limit]:
        print(f"  {name} {category} " f"expect={expected} got={actual}")


def main() -> int:
    verbose = sys.argv[1:] == ["-v"]

    if sys.argv[1:] and not verbose:
        print(f"usage: {Path(sys.argv[0]).name} [-v]", file=sys.stderr)
        return 2

    try:
        metadata = load_metadata()
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"error: failed to load metadata: {error}", file=sys.stderr)
        return 1

    results: dict[str, dict[str, int]] = defaultdict(new_result)

    bad_em64t: list[tuple[str, str, int, Optional[int]]] = []
    bad_llvm: list[tuple[str, str, int, Optional[int]]] = []

    for name in sorted(metadata):
        entry = metadata[name]

        if not isinstance(entry, dict):
            print(
                f"error: metadata entry {name!r} must be an object",
                file=sys.stderr,
            )
            return 1

        try:
            expected = int(entry["expect"])
            category = str(entry["cat"])
        except (KeyError, TypeError, ValueError) as error:
            print(
                f"error: invalid metadata entry for {name!r}: {error}",
                file=sys.stderr,
            )
            return 1

        source = CORPUS_DIR / f"{name}.dc"

        values = {backend: compile_and_run(source, backend) for backend in BACKENDS}

        result = results[category]
        result["n"] += 1

        if values["em"] == expected:
            result["em_ok"] += 1
        else:
            bad_em64t.append((name, category, expected, values["em"]))

        if values["ll"] == expected:
            result["llvm_ok"] += 1
        else:
            bad_llvm.append((name, category, expected, values["ll"]))

        if values["em"] == values["ll"]:
            result["agree"] += 1

    totals = new_result()

    for category in sorted(results):
        result = results[category]

        for key in totals:
            totals[key] += result[key]

        print_result(category, result)

    print_result("TOTAL", totals)

    if verbose:
        print_failures("em64t wrong", bad_em64t, limit=20)
        print_failures("llvm wrong", bad_llvm, limit=10)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
