# A table of types with static if

A generic function usually does the same thing for every `T`. Sometimes the
right code depends on the type: print a boolean as yes/no, treat a float
differently from an integer. `static if` lets the generic function ask
about `T` and keeps only the matching branch in each compiled copy.

```dc
public []const char kind_of(T)() {
    static if T == i32 || T == i64 {
        return "signed";
    } else static if T == f64 {
        return "float";
    } else {
        return "other";
    }
}
```

- The condition compares types with `==`, and `||` joins alternatives.
- `static if sizeof(T) == 1` compares a size, which is also known while
  compiling.
- The function has no value parameters, so the second parentheses are empty
  and the type is written at the call: `kind_of!i32()`.
- Every call compiles only the branch for its type; the other branches are
  not part of that copy.

## Your task

`src/kinds.dc` has two generic functions that return `"?"`. `src/main.dc`
prints a table with them. Implement both with `static if`:

1. `kind_of(T)()` returns `"signed"` for `i8`, `i16`, `i32` and `i64`,
   `"unsigned"` for `u8`, `u16`, `u32` and `u64`, `"float"` for `f32` and
   `f64`, and `"other"` for every other type.
2. `size_class(T)()` returns `"tiny"` when `sizeof(T)` is 1, `"small"` when
   it is at most 4 and `"big"` otherwise.

The program prints:

```text
type  kind      size
i32   signed    small
u8    unsigned  tiny
i64   signed    big
f32   float     small
f64   float     big
bool  other     tiny
char  other     tiny
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The table above matches exactly and the exit code is 0.
- The harness checks pass for the integer and float types that `main` does
  not print.
