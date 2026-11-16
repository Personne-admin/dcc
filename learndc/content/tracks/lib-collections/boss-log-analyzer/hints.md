## Hint 1

Work one stage at a time and run the checks after each. The stages are independent harnesses, so a later stub does not break an earlier stage.

## Hint 2

`parse_line` is the hard part: do the checks in the README's order. `read_log`, `tally` and `latency` then only combine tools you have used in this chapter: split and Array, a stable sort, and a Map whose keys are sorted before use.

## Hint 3

Everything the report needs is already produced by the stages. `main` is only calls and printing; match the formats of the expected output exactly, character by character.
