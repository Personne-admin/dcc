# Decimals and conversions

Whole-number division drops the fraction: the average of 7, 8 and 8 is
23 / 3, and with `i32` that is just `7`. To keep the fraction, the
calculation must be done with `f64` numbers, which have a decimal point.

DC does not mix the two kinds of number silently. Dividing an `i32` by an
`f64` is an error: `binary operator / type mismatch`. You convert a value
yourself with `as`:

```dc
i32 total = 23;
f64 exact = total as f64;
f64 average = total as f64 / 3.0;
```

`total as f64` is the same number as an `f64`: `23.0`. Now the division is
between two `f64` values, so the result keeps its fraction: `7.666666...`.
The literal `3.0` has a decimal point, so it is an `f64` already.

Always write the `as`. A plain `f64 x = some_i32;` does not convert the
number to what you expect, so never leave it out.

## Printing decimals

`{}` prints an `f64` with six digits after the point: `2.500000`. To choose
the number of digits, write `{:.2}` for two:

```dc
println("{:.2}", 2.5)?;
```

prints `2.50`. The value is rounded to the digits you ask for, so `7.6666...`
prints as `7.67`.

## Your task

`src/main.dc` declares all variables at zero and prints them. Calculate them:

- `total` is the sum of the three scores.
- `whole_average` is the total divided by 3 as whole numbers, to see what is
  lost.
- `average` is the same division with the total converted to `f64`.
- `cost` is `price` times `cups`, with `cups` converted to `f64`.

The program must print:

```text
Scores: 7, 8, 8
Total: 23
Integer average: 7
Average: 7.67
Tea costs 2.50, 3 cups cost 7.50
```

Change the declarations only, not the `println` lines.

## Acceptance criteria

- The project builds.
- The program prints exactly the five lines above and exits with code 0.
