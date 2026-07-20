#!/usr/bin/env python3

import itertools
import subprocess
from pathlib import Path
from typing import Optional, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent

DCC = Path("/usr/local/bin/dcc")
LINKER = "ld.lld"

SOURCE_PATH = SCRIPT_DIR / "srch.dc"
OBJECT_PATH = SCRIPT_DIR / "sr.o"
PROGRAM_PATH = SCRIPT_DIR / "srp"
START_OBJECT = SCRIPT_DIR / "start.o"

MAX_FAILURES = 6


STRUCTS: dict[str, list[tuple[str, str]]] = {
    "F8": [("f64", "a")],
    "F16": [("f64", "a"), ("f64", "b")],
    "F24": [("f64", "a"), ("f64", "b"), ("f64", "c")],
    "M8": [("i32", "a"), ("f32", "b")],
    "M16": [("i64", "a"), ("f64", "b")],
    "M16b": [("f64", "a"), ("i64", "b")],
    "M24": [("f64", "a"), ("i64", "b"), ("f64", "c")],
    "S16": [("i64", "a"), ("i64", "b")],
}


def generate_struct_declarations() -> str:
    declarations = []

    for name, fields in STRUCTS.items():
        field_text = " ".join(
            f"{field_type} {field_name};" for field_type, field_name in fields
        )
        declarations.append(f"struct {name} {{ {field_text} }}")

    return "\n".join(declarations)


STRUCT_DECLARATIONS = generate_struct_declarations()


def build(sequence: Sequence[str]) -> tuple[str, int]:
    parameters: list[str] = []
    arguments: list[str] = []
    expressions: list[str] = []
    initializers: list[str] = []

    integer_index = 0
    float_index = 0
    struct_index = 0
    expected = 0

    for item in sequence:
        if item == "i":
            value = integer_index + 1

            parameters.append(f"i32 p{integer_index}")
            arguments.append(str(value))
            expressions.append(f"p{integer_index}")

            expected += value
            integer_index += 1
            continue

        if item == "f":
            value = float_index + 1

            parameters.append(f"f64 q{float_index}")
            arguments.append(f"{value}.0")
            expressions.append(f"q{float_index} as i32")

            expected += value
            float_index += 1
            continue

        try:
            fields = STRUCTS[item]
        except KeyError as error:
            raise ValueError(f"unknown sequence item: {item!r}") from error

        value = struct_index + 1
        parameter_name = f"s{struct_index}"
        argument_name = f"v{struct_index}"

        parameters.append(f"{item} {parameter_name}")
        arguments.append(argument_name)

        field_expressions = " + ".join(
            f"{parameter_name}.{field_name} as i32" for _, field_name in fields
        )
        expressions.append(f"({field_expressions})")

        field_initializers = ", ".join(
            f"{field_name} = {value}{'.0' if field_type.startswith('f') else ''}"
            for field_type, field_name in fields
        )
        initializers.append(f"    {item} {argument_name} = {{ {field_initializers} }};")

        expected += value * len(fields)
        struct_index += 1

    source = f"""\
module t;

{STRUCT_DECLARATIONS}

i32 f({", ".join(parameters)}) {{
    return {" + ".join(expressions)};
}}

@nomangle
public i32 dcc_main() {{
{chr(10).join(initializers)}
    return f({", ".join(arguments)}) & 255;
}}
"""

    return source, expected & 0xFF


def run_command(command: Sequence[str]) -> bool:
    result = subprocess.run(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def check(sequence: Sequence[str]) -> Optional[tuple[int, int]]:
    source, expected = build(sequence)
    SOURCE_PATH.write_text(source, encoding="utf-8")

    OBJECT_PATH.unlink(missing_ok=True)
    PROGRAM_PATH.unlink(missing_ok=True)

    compile_command = [
        str(DCC),
        "-fbackend",
        "em64t",
        "-target",
        "x86_64-elf",
        "-c",
        "-o",
        str(OBJECT_PATH),
        str(SOURCE_PATH),
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
        str(PROGRAM_PATH),
        str(OBJECT_PATH),
        str(START_OBJECT),
    ]

    if not run_command(link_command):
        return None

    result = subprocess.run(
        [str(PROGRAM_PATH)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    return result.returncode, expected


def find_failures() -> list[tuple[list[str], int, int]]:
    pool = ["i", "f", *STRUCTS]
    failures: list[tuple[list[str], int, int]] = []

    for length in (2, 3):
        for combination in itertools.product(pool, repeat=length):
            result = check(combination)

            if result is None:
                continue

            actual, expected = result

            if actual == expected:
                continue

            failures.append((list(combination), actual, expected))

            if len(failures) >= MAX_FAILURES:
                return failures

        if failures:
            break

    return failures


def main() -> int:
    failures = find_failures()

    for sequence, actual, expected in failures:
        print(f"{sequence} got {actual} want {expected}")

    print(f"failures found: {len(failures)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
