## Hint 1

A slice knows its own length (`values.len`) and can be walked with
`for value in values { ... }`. You do not need to be told how many elements
there are.

## Hint 2

`total` keeps a running `i32` sum that starts at 0 and adds each element.
`highest` starts from the first element and replaces it whenever it sees a
bigger one. An empty slice has no first element, so check `values.len == 0`
before reading `values[0]`.

## Hint 3

In `src/scores.dc`, replace the body of `total` with a `sum` variable, a
`for value in values` loop that does `sum += value;`, and `return sum;`. In
`highest`, return 0 when `values.len == 0`, set `best = values[0]`, loop with
`if value > best { best = value; }`, and return `best`.
