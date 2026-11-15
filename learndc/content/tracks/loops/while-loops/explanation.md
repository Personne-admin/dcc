# Why the solution works

```dc
public i32 years_to_double(i32 start, i32 percent) {
    i32 balance = start;
    i32 years = 0;
    while balance < 2 * start {
        balance += balance * percent / 100;
        years += 1;
    }
    return years;
}
```

- The loop does not know how many years it needs. It asks a question before
  every pass: is the balance still below the goal? The answer changes only
  because the body updates `balance`, so the loop ends by itself.
- `years` counts passes. Counting inside the loop and returning after it is
  the most common while-loop shape.
- `count_digits` starts at 1 and only counts the divisions that still leave a
  digit behind. The condition `n >= 10` is what makes `0` correct: no pass
  runs and the answer stays 1. Testing `n > 0` would count 0 as having no
  digits.
- `first_multiple_above` uses `<=` because a multiple equal to the limit is
  not above it: for `(7, 49)` the loop must continue past 49 and stop at 56.
- A condition and its update must fit together. If `years_to_double` were
  called with a `percent` of 0, `balance` would never change and the loop
  would run forever; that is why the task limits the inputs.
