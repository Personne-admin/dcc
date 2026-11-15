# Generics

`larger(i32 a, i32 b)` only works for `i32`. Copying it for every other
number type would be tedious, so DC lets a function take a type as a
compile-time parameter. The type parameters go in their own parentheses
before the ordinary ones:

```dc
public T smaller(T)(T a, T b) {
    return if a < b { a } else { b };
}
```

`T` stands for whatever type the caller uses. The compiler deduces it from
the arguments and generates a separate, fully typed copy for each type that
is actually used:

```dc
smaller(3, 9);            // T is i32
smaller(2.5, 1.25);       // T is f64
smaller!u8(200, 100);     // T given explicitly with !
```

Both arguments must have the same `T`, and the body may use only operations
that every `T` you call it with supports, such as `<` for numbers.

## Your task

`src/pick.dc` has a generic `smaller` and a non-generic `larger` that only
accepts `i32`. The program calls `larger` with decimals and bytes too, so it
does not build yet. Read the diagnostic, then make `larger` generic in the
same way as `smaller`.

## Acceptance criteria

- The project builds with `dpm build`.
- Running it prints exactly:

```text
ints: larger 9, smaller 3
floats: larger 2.500000, smaller 1.250000
bytes: larger 200, smaller 100
```

Run `learndc check` when you are ready, or `learndc run` to see the output.
