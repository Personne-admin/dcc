# Why the solution works

```dc
public i32 total([]const i32 values) {
    i32 sum = 0;
    for value in values {
        sum += value;
    }
    return sum;
}
```

- The parameter is a slice, so the function does not care whether the caller
  has a whole `i32[5]` or a window such as `results[3..5]`. The array converts
  to a slice at the call, and the slice carries its own length, which is why
  the `for` loop knows where to stop.
- An empty slice makes the loop run zero times, so `total` naturally returns
  0 for it.

```dc
public i32 highest([]const i32 values) {
    if values.len == 0 {
        return 0;
    }
    i32 best = values[0];
    ...
}
```

- `highest` reads `values[0]` as its starting candidate. That read is only
  valid when the slice has at least one element, so the empty case is handled
  first. Without the check, a bounds-checked build would panic with
  `array index out of bounds`.
- Starting from the first element, rather than from `0`, keeps the answer
  correct when every value is negative.
