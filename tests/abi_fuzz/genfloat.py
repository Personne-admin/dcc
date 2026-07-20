#!/usr/bin/env python3

import json
import random
from pathlib import Path
from typing import Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
CORPUS_DIR = SCRIPT_DIR / "fcorpus"

RANDOM_SEED = 24601
RANDOM_CASE_COUNT = 250


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
            parameter_name = f"p{integer_index}"

            parameters.append(f"i32 {parameter_name}")
            arguments.append(str(value))
            expressions.append(parameter_name)

            expected += value
            integer_index += 1
            continue

        if item == "f":
            value = float_index + 1
            parameter_name = f"q{float_index}"

            parameters.append(f"f64 {parameter_name}")
            arguments.append(f"{value}.0")
            expressions.append(f"{parameter_name} as i32")

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

        field_sum = " + ".join(
            f"{parameter_name}.{field_name} as i32" for _, field_name in fields
        )
        expressions.append(f"({field_sum})")

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


def generate_fixed_cases() -> list[tuple[str, list[str]]]:
    cases: list[tuple[str, list[str]]] = []

    for count in range(1, 13):
        cases.append((f"float{count:02d}", ["f"] * count))

    cases.extend(
        [
            ("float8_exact", ["f"] * 8),
            ("float9_boundary", ["f"] * 9),
        ]
    )

    for count in (1, 2, 3, 4, 8, 9):
        cases.append(
            (
                f"mix_i{count}_f{count}",
                ["i"] * count + ["f"] * count,
            )
        )

    cases.extend(
        [
            ("mix_interleave_overflow", ["i", "f"] * 7),
            ("mix_int_overflow_float_ok", ["i"] * 9 + ["f"] * 3),
            ("mix_float_overflow_int_ok", ["f"] * 10 + ["i"] * 3),
            ("mix_both_overflow", ["i"] * 8 + ["f"] * 10),
        ]
    )

    for struct_name in STRUCTS:
        cases.extend(
            [
                (f"struct_{struct_name}", [struct_name]),
                (
                    f"struct_{struct_name}_with_floats",
                    ["f"] * 3 + [struct_name] + ["f"] * 2,
                ),
                (
                    f"struct_{struct_name}_overflow",
                    ["i"] * 7 + ["f"] * 9 + [struct_name],
                ),
            ]
        )

    cases.extend(
        [
            ("float_struct_pair", ["F16", "F16"]),
            ("mixed_struct_pair", ["M16", "M16b"]),
        ]
    )

    return cases


def generate_random_cases() -> list[tuple[str, list[str]]]:
    rng = random.Random(RANDOM_SEED)
    pool = ["i", "f", *STRUCTS]

    cases = []

    for index in range(RANDOM_CASE_COUNT):
        length = rng.randint(1, 14)
        sequence = [rng.choice(pool) for _ in range(length)]
        cases.append((f"rand{index:03d}", sequence))

    return cases


def classify(sequence: Sequence[str]) -> str:
    integer_count = sequence.count("i")
    float_count = sequence.count("f")
    struct_count = len(sequence) - integer_count - float_count

    if float_count and not integer_count and not struct_count:
        return "floats"

    if integer_count or float_count:
        return "mixed"

    return "structs"


def clear_corpus_directory() -> None:
    CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    for path in CORPUS_DIR.iterdir():
        if path.is_file() or path.is_symlink():
            path.unlink()


def write_corpus(cases: Sequence[tuple[str, Sequence[str]]]) -> None:
    metadata: dict[str, dict[str, object]] = {}

    for name, sequence in cases:
        source, expected = build(sequence)

        source_path = CORPUS_DIR / f"{name}.dc"
        source_path.write_text(source, encoding="utf-8")

        metadata[name] = {
            "expect": expected,
            "seq": list(sequence),
            "cat": classify(sequence),
        }

    metadata_path = CORPUS_DIR / "meta.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    cases = generate_fixed_cases()
    cases.extend(generate_random_cases())

    clear_corpus_directory()
    write_corpus(cases)

    print(f"generated {len(cases)} float cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
