# Why the solution works

```dc
public Optional(i32) checked_div(i32 a, i32 b) {
    if b == 0 {
        return Optional::None;
    }
    return Optional::Some(a / b);
}
```

- The guard runs before the division, so a zero divisor never reaches `/`.
  The caller gets an ordinary value that says "no answer" instead of a
  crash or a made-up number.
- `Optional::Some(a / b)` wraps the quotient. `Some(0)` is a real answer and
  stays distinct from `None`, which is why returning `0` for "no answer"
  would be wrong.

```dc
match quotient {
    Optional::Some(value) => println("{} / {} = {}", a, b, value)?,
    Optional::None => println("{} / {} = undefined", a, b)?,
}
```

- `match` must be exhaustive: leaving out either arm is a compile error, so
  the "no answer" case cannot be forgotten.
- `value` is bound only inside the `Some` arm; the `None` arm has no payload
  to read.
- Both arms print through `?`, so a failed write still propagates out of
  `show` and `main`.
