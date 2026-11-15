# Why the solution works

```dc
public i32 square(i32 n) {
    return n * n;
}

public i32 sum_of_squares(i32 a, i32 b) {
    return square(a) + square(b);
}

public i32 to_fahrenheit(i32 celsius) {
    return celsius * 9 / 5 + 32;
}

public bool is_even(i32 n) {
    return n % 2 == 0;
}
```

- Parameters are local variables that receive the caller's values, so
  `square(-3)` computes `-3 * -3`, which is 9.
- `sum_of_squares` reuses `square`. A fix or improvement to `square` now
  benefits both functions, and each function stays one line long.
- `*` and `/` have the same precedence and group left to right, so
  `celsius * 9 / 5` multiplies first. Multiplying before dividing keeps
  precision with integers: `20 * 9 / 5` is 180 / 5, which is 36, then plus 32
  gives 68. Dividing first (`celsius / 5 * 9`) would lose the fraction.
- `n % 2` is 0 for even numbers including 0 and negative ones, and
  `n % 2 == 0` is itself a `bool`, so it can be returned directly with no
  `if`.
- `main.dc` only calls the public functions. Because `numbers` lives in its
  own module, tests can import it without running `main`.
