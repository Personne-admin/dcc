# The Number Oracle

The oracle's methods live in `src/oracle.dc` as functions that return
placeholders; `src/main.dc` is where she prints her report. Each stage below is
checked on its own, so you can clear them in order and watch them light up.

Every function takes plain numbers; use the loops of this chapter and the
conditions of the last one. Parameters are copies, so a function may change its
own parameters while it works.

## Stage 1: Read the old page

Before you write anything, a question about the apprentice's legacy page
asks you to find a bug in his `is_prime`. It is the same kind of mistake you
traced in the lecture.

## Stage 2: Sum of the digits

`digit_sum(n)` adds the decimal digits of `n` (for `n >= 0`):
`digit_sum(1234)` is 10, `digit_sum(0)` is 0.

## Stage 3: Prime or not

`is_prime(n)` is `true` for primes and `false` otherwise. A prime is a number
of at least 2 that has no divisor except 1 and itself. Numbers below 2, such
as 1, 0 and negative numbers, are not prime. The checks use numbers up to about
8000, so trying every divisor is fast enough.

## Stage 4: The Collatz journey

`collatz_steps(n)` (for `n >= 1`) applies this rule until the number is 1,
and counts how often it did: an even number is halved, an odd number becomes
`3 * n + 1`. For example 6 goes 6, 3, 10, 5, 16, 8, 4, 2, 1: that is 8
steps. `collatz_steps(1)` is 0.

## Stage 5: The greatest common divisor

`gcd(a, b)` is the largest number that divides both `a` and `b`. Euclid's
method: while `b` is not 0, replace `a` by `b` and `b` by the remainder
`a % b`. When `b` is 0, `a` is the answer. For example `gcd(12, 18)` goes
(12, 18), (18, 12), (12, 6), (6, 0) and the answer is 6.

## Stage 6: The oracle speaks

Complete `main` in `src/main.dc`. After the two header lines it already prints,
print one row for every `n` from 1 to 15: the number, its digit sum, whether it
is prime, its Collatz steps and its `gcd` with 12. Right-align each value with
a width, using `{:>3}`, `{:>9}`, `{:>6}`, `{:>8}` and `{:>11}` for the five
columns, with one space between them. Finally print how many primes there
were. The first and last lines of the output:

```text
number oracle
  n digit sum  prime  collatz  gcd(n, 12)
  1         1  false        0           1
  2         2   true        1           2
...
 15         6  false       17           3
primes up to 15: 6
```

## Bonus: perfect numbers

`is_perfect(n)` is `true` when `n` equals the sum of its divisors smaller than
itself: 6 is perfect (1 + 2 + 3), and so are 28 and 496. The bonus is not
needed to defeat the boss.
