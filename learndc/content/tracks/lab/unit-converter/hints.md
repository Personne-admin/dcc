## Hint 1

Do the stages in order. Each one builds on the previous: lookup, then the two
formulas, then `convert` which uses both, then two printing functions that
call `convert`.

## Hint 2

When a harness line says FAIL, compute that case by hand with the formula:
for 100 C to F the base value is `100 * 1 + 273.15 = 373.15` and the
result is `(373.15 - 255.3722) / 0.5556 = 212`.

## Hint 3

Floating point numbers are rarely exactly equal, which is why the harness
accepts a tiny difference. For your own comparisons do the same, and print
with `{:.2}` or `{:.3}` to see the rounded value.
