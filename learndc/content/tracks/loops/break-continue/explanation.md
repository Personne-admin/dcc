# Why the solution works

```dc
public i32 smallest_divisor(i32 n) {
    i32 found = n;
    for i32 d in 2..=n {
        if n % d == 0 {
            found = d;
            break;
        }
    }
    return found;
}
```

- The loop tries 2, 3, 4, ... in order, so the first `d` that divides `n` is
  the smallest one. Without `break` the loop would keep going and `found`
  would end up as the *largest* divisor, `n` itself.
- `found` starts at `n`. Since the range includes `n`, `n % n == 0` would be
  found anyway, but the start value documents the answer for a prime.
- `sum_skipping_threes` reads like the task: for each number, skip it if it is
  a multiple of 3, else add it. The `continue` keeps the `total += i` line
  free of an extra `if`.
- `terms_to_reach` cannot know in advance how many terms it needs, so the loop
  has no natural end. The `break` inside is the only exit, and it is
  reachable because `sum` grows with every pass.
