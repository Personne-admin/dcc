# Variables and types

A variable has a type, a name and a value. In DC the type comes first:

```dc
i32 count = 3;
[]const char word = "tree";
```

`i32` is a 32-bit signed integer. `[]const char` is borrowed text. The value
you store must fit the declared type: text cannot go into an `i32`, and a
number cannot go into a `[]const char`. The compiler rejects the mismatch
before the program ever runs.

A variable can be changed after it is declared with `=`:

```dc
count = count + 1;
```

## Your task

`src/main.dc` is meant to print a report about a rectangle that is 12 wide
and 5 high:

```text
Rectangle: 12 x 5
Area: 60
```

It does not build yet, and once it does, it prints the wrong numbers. Find
both problems:

1. Read the `error:` message from `dpm build` and fix the line it points to.
2. Make `width` 12 and `height` 5. Leave the line that computes `area` as it
   is; it will then produce 60.

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints exactly the two lines above and exits with code 0.

Run `learndc check` when you are ready, or `learndc run` to see the output.
