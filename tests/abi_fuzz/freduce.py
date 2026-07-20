#!/usr/bin/env python3

import json
import subprocess
import sys
from pathlib import Path
from typing import Optional, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
DCC = Path("/usr/local/bin/dcc")

SOURCE_PATH = SCRIPT_DIR / "red2.dc"
OBJECT_PATH = SCRIPT_DIR / "r2.o"
PROGRAM_PATH = SCRIPT_DIR / "r2p"
MINIMIZED_PATH = SCRIPT_DIR / "fmin.dc"
START_OBJECT = SCRIPT_DIR / "start.o"


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


def build(sequence: Sequence[str]) -> tuple[Optional[str], int]:
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
        variable = f"s{struct_index}"

        parameters.append(f"{item} {variable}")
        arguments.append(f"v{struct_index}")

        field_expressions = " + ".join(
            f"{variable}.{field_name} as i32" for _, field_name in fields
        )
        expressions.append(f"({field_expressions})")

        field_initializers = ", ".join(
            f"{field_name} = {value}{'.0' if field_type.startswith('f') else ''}"
            for field_type, field_name in fields
        )
        initializers.append(f"    {item} v{struct_index} = {{ {field_initializers} }};")

        expected += value * len(fields)
        struct_index += 1

    if not parameters:
        return None, 0

    initializer_text = "\n".join(initializers)
    parameter_text = ", ".join(parameters)
    argument_text = ", ".join(arguments)
    expression_text = " + ".join(expressions)

    source = f"""\
module t;

{STRUCT_DECLARATIONS}

i32 f({parameter_text}) {{
    return {expression_text};
}}

@nomangle
public i32 dcc_main() {{
{initializer_text}
    return f({argument_text}) & 255;
}}
"""

    return source, expected & 0xFF


def command_succeeds(command: Sequence[str]) -> bool:
    result = subprocess.run(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def reproduces(sequence: Sequence[str]) -> bool:
    source, expected = build(sequence)

    if source is None:
        return False

    SOURCE_PATH.write_text(source, encoding="utf-8")

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

    if not command_succeeds(compile_command):
        return False

    link_command = [
        "ld.lld",
        "--static",
        "--no-dynamic-linker",
        "-e",
        "_start",
        "-o",
        str(PROGRAM_PATH),
        str(OBJECT_PATH),
        str(START_OBJECT),
    ]

    if not command_succeeds(link_command):
        return False

    result = subprocess.run(
        [str(PROGRAM_PATH)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    return result.returncode != expected


def minimize(sequence: list[str]) -> list[str]:
    while True:
        for index in range(len(sequence)):
            candidate = sequence[:index] + sequence[index + 1 :]

            if candidate and reproduces(candidate):
                sequence = candidate
                break
        else:
            return sequence


def parse_sequence(argument: str) -> list[str]:
    value = json.loads(argument)

    if not isinstance(value, list):
        raise ValueError("sequence must be a JSON array")

    if not all(isinstance(item, str) for item in value):
        raise ValueError("every sequence item must be a string")

    return value


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} '<json-sequence>'", file=sys.stderr)
        return 2

    try:
        sequence = parse_sequence(sys.argv[1])
    except (json.JSONDecodeError, ValueError) as error:
        print(f"invalid sequence: {error}", file=sys.stderr)
        return 2

    if not sequence:
        print("sequence must not be empty", file=sys.stderr)
        return 2

    if not reproduces(sequence):
        print("error: input sequence does not reproduce", file=sys.stderr)
        return 1

    minimized = minimize(sequence)
    source, expected = build(minimized)

    assert source is not None

    MINIMIZED_PATH.write_text(source, encoding="utf-8")
    print(f"minimal: {minimized} expect {expected}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
