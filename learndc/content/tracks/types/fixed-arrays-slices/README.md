# Fixed arrays and slices

A fixed array owns a known number of elements and has that size in its type:
`i32[5]` is exactly five `i32` values stored in place. A slice, written
`[]const i32`, is a borrowed view of some elements: a pointer plus a length.
It does not own anything and is the usual way to pass "some numbers" to a
function.

```dc
i32[5] results = { 12, 7, 31, 4, 19 };
[]const i32 last_two = results[3..5];   // elements 3 and 4
println("{} {}", last_two.len, last_two[0])?;   // 2 4
```

- `a[start..end]` makes a slice; `end` is exclusive.
- `.len` is the number of elements, `values[i]` reads one (counting from 0).
- A whole array converts to a slice when passed to a function that takes one.
- `for value in values { ... }` visits every element.
- Indexing is bounds-checked in `dpm` builds: reading past the end stops the
  program with `panic: array index out of bounds` instead of reading junk.

## Your task

`src/main.dc` already builds the array and prints a summary. Implement the two
functions in `src/scores.dc`, which currently return `0`:

- `total` returns the sum of all values in the slice.
- `highest` returns the largest value, or `0` for an empty slice.

Both must work for any slice, whether it came from a whole array or from
`results[3..5]`.

## Acceptance criteria

- The project builds with `dpm build`.
- Running it prints exactly:

```text
all: total 73, highest 31
last two: total 23, highest 19
```

- `total` and `highest` also give correct answers for empty slices and for
  negative numbers.

Run `learndc check` when you are ready, or `learndc run` to see the output.
