import argparse
import collections
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(
    description=(
        "Summarize saved Callgrind self costs and calls; "
        "verify cost conservation."
    )
)
parser.add_argument("input", type=Path)
parser.add_argument("--output", type=Path)

args = parser.parse_args()

data = json.loads(args.input.read_text())

artifacts = (
    args.input.resolve().parent
    / f"{args.input.stem}-artifacts"
)

result = []

for r in data["results"]:
    profile_path = (
        artifacts
        / r["benchmark"]
        / r["config"].replace(":", "-")
        / "callgrind.out"
    )

    profile = r.get("profile", {})

    if not profile_path.exists() or profile.get("status") != "ok":
        continue

    lines = profile_path.read_text().splitlines()

    names = {}
    self_ir = collections.Counter()
    calls = collections.Counter()

    for line in lines:
        match = re.match(r"c?fn=\((\d+)\) (.*)", line)
        if match:
            names[match.group(1)] = match.group(2)

    current = None
    callee = None
    pending_call_cost = False

    positions_line = next(
        line for line in lines
        if line.startswith("positions:")
    )
    positions = len(positions_line.split()) - 1

    for line in lines:
        match = re.match(r"(c?fn)=\((\d+)\)", line)

        if match:
            function_type, function_id = match.groups()

            if function_type == "fn":
                current = names[function_id]
            else:
                callee = names[function_id]

        elif line.startswith("calls="):
            call_count = int(line.split()[0].split("=")[1])
            calls[callee] += call_count
            pending_call_cost = True

        elif line and (line[0].isdigit() or line[0] in "*+-"):
            if pending_call_cost:
                pending_call_cost = False
                continue

            fields = line.split()

            if current and len(fields) > positions:
                self_ir[current] += int(fields[positions])

    events = r["profile"]["events"]

    small_functions = {
        name
        for name, size in r["functions_after"].items()
        if 0 < size <= 20
    }

    small_std_functions = {
        name
        for name in small_functions
        if re.match(r"^_DC0[FS]\d+\.3\.std", name)
    }

    small_self_instructions = sum(
        self_ir[name] for name in small_functions
    )
    small_dynamic_calls = sum(
        calls[name] for name in small_functions
    )

    hardware = {}

    for line in r.get("perf", {}).get("raw", "").splitlines():
        fields = line.split(",")

        if len(fields) > 2 and fields[0].isdigit():
            event_name = fields[2].removesuffix(":u")
            hardware[event_name] = int(fields[0])

    conserved = int(
        next(
            line for line in lines
            if line.startswith("totals:")
        ).split()[1]
    )

    self_sum = sum(self_ir.values())

    if self_sum != conserved:
        raise ValueError(
            f"Unaccounted Callgrind self costs: {profile_path}"
        )

    total_instructions = events["Ir"]

    item = {
        "small_std_self_instructions": sum(
            self_ir[name] for name in small_std_functions
        ),
        "small_std_dynamic_calls": sum(
            calls[name] for name in small_std_functions
        ),
        "benchmark": r["benchmark"],
        "config": r["config"],
        "events": events,
        "hardware": hardware,
        "small_self_instructions": small_self_instructions,
        "small_dynamic_calls": small_dynamic_calls,
        "total_dynamic_calls": sum(calls.values()),
        "self_sum": self_sum,
        "top_self": [
            {
                "symbol": name,
                "instructions": instructions,
                "percent": 100 * instructions / total_instructions,
                "calls": calls[name],
            }
            for name, instructions in self_ir.most_common(12)
        ],
        "all_self": dict(self_ir),
        "all_calls": dict(calls),
    }

    result.append(item)

    memory_accesses = events["Dr"] + events["Dw"]
    branches = max(hardware.get("branches", 1), 1)
    branch_misses = hardware.get("branch-misses", 0)

    print(
        r["benchmark"],
        r["config"],
        "small self",
        round(100 * small_self_instructions / total_instructions, 2),
        "small calls",
        small_dynamic_calls,
        "total calls",
        sum(calls.values()),
        "mem/instr",
        round(memory_accesses / total_instructions, 2),
        "branch miss%",
        round(100 * branch_misses / branches, 4),
    )

    for name, instructions in self_ir.most_common(4):
        print(
            " ",
            round(100 * instructions / total_instructions, 2),
            name,
        )

output_path = args.output or args.input.with_suffix(".profiles.json")
output_path.write_text(json.dumps(result, indent=2) + "\n")
