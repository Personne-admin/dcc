# Why the solution works

```dc
public bool is_prime(i32 n) {
    if n < 2 {
        return false;
    }
    for i32 d in 2..n {
        if d * d > n {
            break;
        }
        if n % d == 0 {
            return false;
        }
    }
    return true;
}
```

- Each function uses the loop that matches the question. `is_prime` knows its
  candidates (2 up to just below n), so it uses `for`. `collatz_steps` and
  `gcd` cannot know how many passes they need, so they use `while` with a
  condition that the body works towards.
- `is_prime` returns as soon as it has a divisor, and `true` only after the
  loop found none. The range `2..n` excludes `n`: with `2..=n` every number
  would divide itself and nothing would be prime, which is the bug on the old
  page. The `break` once `d * d > n` is an optional speed-up: if `n` has a
  divisor above its square root, it has a matching one below it, so the search
  can stop early.
- `digit_sum` and `gcd` change their parameters (`n /= 10`, `a = b`). That is
  fine: parameters are copies, and the caller's variable is untouched.
- `gcd` works because `gcd(a, b)` equals `gcd(b, a % b)` and the remainder gets
  smaller every pass until it is 0, so the loop always ends.
- `collatz_steps` has no proof that it always ends for every number, but it
  does for all numbers anyone has tried; the checks only use small inputs.
- In `main`, the loop calls the functions once per row, and `{:>N}` pads each
  value on the left to width N so that the columns line up. `is_prime(n)` is
  called twice (for the row and for the counter); storing it in a `bool`
  variable first would avoid the repeat.
