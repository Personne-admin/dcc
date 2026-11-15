# Why the solution works

```dc
for i in 1..=n {
    total += i;
}
```

- `1..n` is half-open and never visits `n`, so `sum_to(5)` added only 1, 2, 3
  and 4. The wrong answers were always `n` too small, which is the signature
  of an off-by-one error: the pattern in the output points to the loop bound.
- `1..=n` includes `n`, so the loop visits 1 through `n`. `1..n + 1` would
  also work, but `..=` says what you mean.
- Edge cases come for free. For `n = 0` or a negative `n` the range `1..=n`
  is empty, so the body never runs and `total` keeps its initial 0. Always
  check the smallest inputs after changing a bound.
- The range bound `n` is an `i32`, so `i` is an `i32` and `total += i` needs
  no conversion. Ranges built from plain literals alone default to `usize`.
- The `while` countdown shows the other loop style: the condition `count > 0`
  is tested before every iteration, and `count -= 1` makes sure it eventually
  becomes false.
