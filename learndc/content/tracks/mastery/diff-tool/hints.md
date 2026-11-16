## Hint 1

The table `L` is filled backwards: `L(i, j)` depends on `L(i + 1, j + 1)`,
`L(i + 1, j)` and `L(i, j + 1)`, so start at the end of both sequences. A
two-dimensional table of `n + 1` by `m + 1` numbers fits in one flat array with
`i * (m + 1) + j`. The extra row and column of zeros are what make the empty
tails work without special cases.

## Hint 2

Both `lcs_length` and `diff` need the same table: write one private generic
helper that fills an `Array(u32)` and use it twice. Check the size first with a
division instead of a multiplication, for example whether `m + 1` is at most
`MAX_CELLS / (n + 1)`, so the check itself cannot overflow. For `intern`, a plain
search through the table is enough.

## Hint 3

`diff` is the walk described in the README with `i` and `j` and the table, followed by
two loops for the leftovers. Print inside the `match` arms in `main` and keep three
counters. The generic function is called like any other: `diff(a.items(), b.items(),
&script)` deduces `T = u32` from the slices. Remember `defer` for every array
you create, including the table.
