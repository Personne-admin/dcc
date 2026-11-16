## Hint 1

Build the machine in layers and test each: one line, many lines, one run, a
listing, and only then the program that wires them together. Each layer takes
the one before as given.

## Hint 2

Both `parse_instruction` and `run` are long, but only because there are many
instructions. Decide the common structure first (a few checks, then a match or
an if chain) and add instructions one at a time, running the harness after each.

## Hint 3

For `run`, put the decision about the next program counter inside the `match`:
start with `next = pc + 1` and let only `Jump` and `JumpIfZero` change it. The
state that must survive a failed instruction is the stack and `out`, so check
before you pop.
