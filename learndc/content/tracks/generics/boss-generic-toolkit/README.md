# The Generic Toolkit

The station and the stall need the same tools for different number types.
Write them once, generic, in two modules, and use them from `main`. Work
stage by stage; every stage has its own checks and hints.

## What you are building

`src/toolkit.dc` has generic functions over slices. The signatures are
already there and compile, but return placeholder values:

- `max_of(items)` and `min_of(items)`: the largest and smallest element of a
  non-empty slice.
- `count_above(items, limit)`: how many elements are greater than `limit`.
- `count_between(items, low, high)`: how many elements are in `low..=high`.
- `find_sorted(sorted, key)`: the index of `key` in an ascending slice, as
  `Optional(usize)`.
- `clamp_all(items, low, high)` (bonus): limit every element to the range,
  in place.

`src/stack.dc` holds `Stack(T)`, a fixed-capacity stack of 8 values. Only its
shape is given: you decide the fields and write `push` (returns `false`
when full), `pop` (returns `Optional(T)`) and `size`.

`src/main.dc` is the report. It already has the data:

```dc
i32[6] temperatures = { 12, -4, 31, 25, 8, 22 };
f64[4] prices = { 2.5, 9.5, 0.75, 6.0 };
```

and has to print exactly:

```text
temperatures: max 31, min -4
prices: max 9.50, min 0.75
above 20: 3
above 5.00: 2
between 10 and 25: 3
reversed: 22 8 25 31 -4 12
sorted: -4 8 12 22 25 31
25 found at 4
30 not found
```

The `reversed` line comes from pushing the temperatures onto a `Stack(i32)`
and popping them all. The `sorted` line is `std::sort::sort` on the
temperatures (already imported in `main.dc`) and the last two lines use
`find_sorted` on the sorted array.

## Rules

- Every toolkit function is generic: the same code must work for `i32`,
  `f64`, `u8` and `char`.
- Do not print from generic functions; return the answers.
- With a `u8` array and a literal, name the type: `count_above!u8(bytes, 100)`.
- Do not change the signatures of the toolkit functions.

## Acceptance criteria

- The project builds with `dpm build`.
- The report above matches exactly and the exit code is 0.
- Every stage's checks pass; the bonus stage is optional.
