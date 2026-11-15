## Hint 1

Everything after the colon in `{:...}` is a layout spec. The header line
already uses `{:<8}` and `{:>5}`, so you can copy the idea for each column.

## Hint 2

Give each of the three row placeholders the same width and alignment as its
header column, and add `.2` to the price. In the footer, a leading `0` before
the width pads with zeros, and a trailing letter selects the base: `b` for
binary, `X` for upper-case hexadecimal.

## Hint 3

In `row`, the format string becomes `"{:<8}{:>5}{:>9.2}"`. In `footer`, it
becomes `"id {:04} flags {:08b} mask {:X}"`. Leave the arguments as they are.
