# Your first functions

A function is a named recipe: a signature that says what goes in and what
comes out, and a body that does the work.

```dc
public i32 twice(i32 n) {
    return n * 2;
}
```

Here `i32` is the return type, `twice` the name and `i32 n` the one
parameter. `public` lets other files call the function; leave it on every
function in this exercise. `src/main.dc` already begins with
`import basics; using basics::*;`, which is the scaffolding that lets it call
the functions in `src/basics.dc`.

## Your task

1. Fill in the bodies of `triple` and `rectangle_area` in `src/basics.dc`.
   `twice` is finished and shows the pattern.
2. Write a whole new function at the end of `src/basics.dc` from this
   description: `seconds_in` takes a number of minutes (an `i32`) and returns
   the number of seconds (an `i32`) in that many minutes.
3. Add a line to `main` in `src/main.dc` that prints
   `3 minutes = 180 seconds` by calling your new function with `println`.
   Put it after the area line.

The program must print exactly:

```text
twice(21) = 42
triple(14) = 42
area of 6 x 7 = 42
3 minutes = 180 seconds
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output above, line for line.
- `triple`, `rectangle_area` and `seconds_in` return the right values for
  other inputs too, including 0 and negative numbers.
