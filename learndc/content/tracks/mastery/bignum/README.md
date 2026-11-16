# Mastery: Big Numbers

Needs: the whole core curriculum. This challenge is about representation,
carrying, memory and the details that make arithmetic exact. There is almost no
scaffolding: the module `bignum` declares the interface and you write the bodies.

`u64` stops at 18446744073709551615, and `100!` has 158 digits. A *big number*
stores the digits in an `Array(u32)` of *limbs*:

- each limb is a base-1,000,000,000 digit (nine decimal digits), so every limb is
  below `BASE = 1000000000`,
- the first element is the **least significant** limb,
- a number is **normal** when it has no leading zero limbs. The number 0 is the single
  limb `0`, never an empty array.

Concretely: 123456789012345678 = 123456789 * 10^9 + 012345678, so its limbs are
`[12345678, 123456789]`, and 1000000000 is `[0, 1]`:

```text
number               limbs (least significant first)
7                    [7]
1000000000           [0, 1]
123456789012345678   [12345678, 123456789]
```

Every function must return numbers in normal form. The harness checks it.

```dc
public const u32 BASE = 1000000000;

public enum BigError {
    @implicit_construction Alloc(AllocError),
    Underflow,
    DivideByZero,
}
```

## Functions

Numbers are always passed as `Array(u32)*` (or `const Array(u32)*` when only read).
Functions that can grow a number return `Status(AllocError)`.

| function | contract |
| --- | --- |
| `set_u64(n, value)` | `n` becomes `value`, whatever it held before |
| `to_text(n, out)` | `out` is cleared and receives the decimal digits, no leading zeros, `"0"` for zero |
| `compare(a, b)` | -1, 0 or 1 |
| `add(a, b)` | `a = a + b`. `b` may be the same array as `a` (`add(&x, &x)` doubles) |
| `mul_small(a, m)` | `a = a * m` for any `u32` `m`, including 0 |
| `mul(a, b, out)` | `out = a * b`, `out` cleared first and different from `a` and `b` |
| `sub(a, b)` | `a = a - b`, or `Underflow` with `a` unchanged when `a < b`. `b` may be `a` |
| `divmod_small(a, d)` | `a = a / d`, returns the remainder; `DivideByZero` with `a` unchanged when `d == 0` |

Hints about the arithmetic itself: add the limbs of the two numbers plus a carry in
a `u64`, keep `sum % BASE` and carry `sum / BASE`. A limb times a `u32` plus a carry
can exceed 32 bits, so widen *before* you multiply. Long division goes from the
most significant limb down, with the remainder of one step multiplied by `BASE` and
added to the next limb.

The hazard in `add(&x, &x)` is that you read from `b` while you push onto `a`, and
a push can move the storage. Read with `items()` at the moment you need the value,
not before.

## The report

`src/main.dc` computes and prints, in this exact format:

```text
25! = 15511210043330985984000000
2^100 = 1267650600228229401496703205376
2^200 = 1606938044258990275541962092341162602522202993782792835301376
fib(100) = 354224848179261915075
100! has 158 digits, digit sum 648
2^1000 has 302 digits, digit sum 1366
(2^64 - 1) * 2 / 7 = 5270498306774157604 remainder 2
1000 - 2000: underflow
```

`2^200` is computed with `mul` (2^100 times itself), the others with `mul_small`,
`add` (Fibonacci: two numbers you keep replacing) and `divmod_small`. The last
line is the `Underflow` of `sub`.

## Acceptance criteria

- The project builds with `dpm build`.
- Every check passes, including the exact report from `main`.
