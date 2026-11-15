# Loops and ranges

A `for` loop visits every value of a range:

```dc
for i in 0..3 {
    println("{}", i)?;
}
```

`0..3` is a half-open range: it includes the start and excludes the end, so
the loop above prints 0, 1 and 2. Write `0..=3` for an inclusive range that
also visits 3. When the end is an `i32` variable the loop variable is an
`i32` too.

A `while` loop repeats as long as its condition stays `true`; you change the
state yourself inside the body:

```dc
i32 count = 3;
while count > 0 {
    count -= 1;
}
```

Mixing up `..` and `..=` is the classic off-by-one error: the program runs,
no error is reported, and the answer is slightly wrong.

## Your task

`sum_to(n)` in `src/sums.dc` should return `1 + 2 + ... + n`, so
`sum_to(10)` is 55. The program builds, but prints 45. Find the mistake in
the loop and fix it. The program must also keep working for `n` equal to 1,
0 and negative numbers, where the sum is 1 or 0.

The countdown in `src/main.dc` already works; leave it alone.

## Acceptance criteria

- The program prints exactly:

```text
Sum of 1 to 1 is 1
Sum of 1 to 5 is 15
Sum of 1 to 10 is 55
3...
2...
1...
Liftoff!
```

- `sum_to` passes the function check for 1, 2, 5, 10 and 100, and also
  returns 0 for 0 and -3.
