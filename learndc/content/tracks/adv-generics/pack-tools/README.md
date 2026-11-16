# Variadic tools

`src/tools.dc` holds four variadic functions. The signatures are final and
compile, but the bodies return placeholders:

| function | result |
|---|---|
| `sum_all(args...)` | the sum of all arguments as `f64`; zero for no arguments |
| `largest_of(first, rest...)` | the largest argument as `f64` (at least one argument) |
| `count_negative(args...)` | how many arguments are below zero, as `usize` |
| `log_line(tag, args...)` | prints `[tag]` and the arguments, separated by `, ` |

Every argument may have its own type (`i32`, `f64`, `u8`, ...), except in
`log_line`, where any printable type is allowed. Convert numbers with
`item as f64` inside `static for item in args`.

`largest_of` takes a first parameter and a pack for the rest. The first
argument is the starting candidate, so the function can never be called
with nothing and never needs a made-up starting value.

`log_line` prints `[tag]`, then the first element after a space, every
other element after a comma and a space, and finally a newline. A call with
only a tag prints `[tag]`. Track "is this the first element?" with a `bool`
that you update inside the unrolled loop.

`main.dc` is fixed and must produce:

```text
pack tools
[sum] 1, 2.500000, 3
total: 6.5
largest: 3.0
negatives: 2
[empty]
[mixed] text, c, 42, true
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The report above matches exactly and the exit code is 0.
- The harness checks pass: mixed types, no arguments and negative values.
