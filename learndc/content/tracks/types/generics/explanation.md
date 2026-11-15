# Why the solution works

```dc
public T larger(T)(T a, T b) {
    return if a > b { a } else { b };
}
```

- `(T)` introduces a type parameter. In `larger(2.5, 1.25)` the compiler sees
  two `f64` arguments, deduces `T = f64`, and compiles a version of the
  function whose parameters and result are `f64`. `larger(low, high)` with
  `u8` variables compiles a separate `u8` version.
- The body is checked against the actual type for each use. `a > b` is valid
  for numbers, so the same text works for `i32`, `u8`, `u64` and `f64`. A
  type without `>` would be rejected at the call that uses it.
- Both parameters share the one `T`, so the two arguments must have the same
  type. That is why the result type `T` is always known and why the call
  cannot mix a `u8` with an `f64`.
- Literals such as `3` and `9` have no type of their own until the context
  gives one; with nothing else to go on, they default to `i32`, which is how
  `larger(3, 9)` works. If you want another type, either use typed variables
  or give `T` explicitly with `larger!u8(3, 9)`.
