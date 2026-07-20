#!/usr/bin/env python3

import json
import random
from pathlib import Path
from typing import Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
CORPUS_DIR = SCRIPT_DIR / "corpus"
RANDOM_SEED = 31337
RANDOM_CASE_COUNT = 300


STRUCTS: dict[str, list[tuple[str, str]]] = {
    "S4": [("i32", "a")],
    "S8": [("i32", "a"), ("i32", "b")],
    "S12": [("i32", "a"), ("i32", "b"), ("i32", "c")],
    "S16": [("i64", "a"), ("i64", "b")],
    "S20": [
        ("i32", "a"),
        ("i32", "b"),
        ("i32", "c"),
        ("i32", "d"),
        ("i32", "e"),
    ],
    "S24": [("i64", "a"), ("i64", "b"), ("i64", "c")],
    "S32": [
        ("i64", "a"),
        ("i64", "b"),
        ("i64", "c"),
        ("i64", "d"),
    ],
    "S40": [
        ("i64", "a"),
        ("i64", "b"),
        ("i64", "c"),
        ("i64", "d"),
        ("i64", "e"),
    ],
}


FIXED_CASES: list[list[str]] = [
    ["i"] * 4,
    ["i"] * 6,
    ["i"] * 7,
    ["i"] * 8,
    ["i"] * 9,
    ["i"] * 12,
    ["S24"],
    ["S32"],
    ["S12"],
    ["S4"],

    [
        "i",
        "i",
        "i",
        "i",
        "S4",
        "S12",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
    ],
    [
        "i",
        "i",
        "S16",
        "i",
        "i",
        "S4",
        "S12",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
    ],
    [
        "S16",
        "S32",
        "i",
        "i",
        "i",
        "S20",
        "i",
        "i",
        "i",
        "i",
        "i",
    ],
    ["i"] * 6 + ["S24"],
    ["i"] * 7 + ["S4"],
    ["i"] * 4 + ["S8", "S8"],
]


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
            f"{parameter_name}.{field_name}" for _, field_name in fields
        )
        expressions.append(f"({field_sum}) as i32")

        field_initializers = ", ".join(
            f"{field_name} = {value}" for _, field_name in fields
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


def generate_random_case(rng: random.Random) -> list[str]:
    integer_count = rng.randint(0, 11)
    struct_count = rng.randint(0, 3)

    if integer_count == 0 and struct_count == 0:
        integer_count = 1

    sequence = ["i"] * integer_count
    sequence.extend(rng.choice(tuple(STRUCTS)) for _ in range(struct_count))

    rng.shuffle(sequence)
    return sequence


def generate_cases() -> list[list[str]]:
    rng = random.Random(RANDOM_SEED)
    cases = [sequence.copy() for sequence in FIXED_CASES]

    for _ in range(RANDOM_CASE_COUNT):
        cases.append(generate_random_case(rng))

    return cases


def classify(sequence: Sequence[str]) -> str:
    integer_count = sequence.count("i")
    struct_count = len(sequence) - integer_count

    if struct_count == 0:
        return "ints_only"

    if integer_count == 0:
        return "structs_only"

    return "mixed"


def clear_corpus_directory() -> None:
    CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    for path in CORPUS_DIR.iterdir():
        if path.is_file() or path.is_symlink():
            path.unlink()


def write_corpus(cases: Sequence[Sequence[str]]) -> None:
    metadata: dict[str, dict[str, object]] = {}

    for index, sequence in enumerate(cases):
        source, expected = build(sequence)
        name = f"c{index:03d}"

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
    cases = generate_cases()

    clear_corpus_directory()
    write_corpus(cases)

    print(f"generated {len(cases)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
