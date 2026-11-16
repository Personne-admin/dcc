# Numbers from text

Input arrives as text. The library has no one-call "text to integer"
function; it gives you the careful building blocks that its own JSON and TOML
readers use, in `std::parse`:

```dc
import std::parse;

usize cursor = 0;        // where to start; the call moves it past the digits
usize digits = 0;        // how many digits were taken
Result(u64, std::parse::NumError) r =
    std::parse::accumulate_digits(text, &cursor, 10, false, &digits);
```

- `accumulate_digits(text, &cursor, base, underscores, &count)` reads digits
  from `text[cursor..]`, stops at the first non-digit and returns the value as
  a `u64`. It fails with `NoDigits` (nothing to read), `OutOfRange` (does not
  fit in a `u64`) and, with `underscores` set, `BadUnderscore`.
- It does **not** insist that the digits are the whole text. After the call,
  `cursor` tells how far it got: for `12ab` the value is 12 and the cursor is
  2. Checking `cursor == text.len` is your job.
- `std::parse::signed_from_magnitude(magnitude, negative)` turns the `u64`
  and a sign into an `i64`, failing with `OutOfRange` when it does not fit.
  It accepts the magnitude 9223372036854775808 only for a negative number, so
  `-9223372036854775808` works and `9223372036854775808` does not.

The sign is yours to read: look at the first character yourself, and start
the cursor after it.

A library error such as `NumError` is rarely what your caller wants to see.
Map it into an enum that describes **your** function's failures
(`ParseFail`: `Empty`, `NotANumber`, `TooBig`) with a `match`.

## Your task

`src/main.dc` declares `ParseFail`, a finished `report` that prints one parse
result and a finished `main`. Implement:

1. `parse_i64(text)`: an empty text is `Empty`; an optional `-` or `+` is read
   first; then the digits. No digits, or characters left over after the
   digits, is `NotANumber`. A value that does not fit in an `i64` is `TooBig`.
2. `sum_line(line)`: splits `line` on spaces (`std::slice::split(line, ' ' as char)`
   gives an iterator whose `next()` returns an `Optional` text), skips empty
   tokens caused by repeated spaces, adds up `parse_i64` of every other token and
   stops at the first token that fails, passing its error on with `?`.

Expected output:

```text
                    42 -> 42
                   -17 -> -17
                    +8 -> 8
                       -> empty
                     - -> not a number
                  12ab -> not a number
  99999999999999999999 -> too big
   9223372036854775807 -> 9223372036854775807
  -9223372036854775808 -> -9223372036854775808
  -9223372036854775809 -> too big
sum of '3 4 5' is 12
sum of '10 -4  1' is 7
'7 x 2' has a bad number
'1 99999999999999999999' has a bad number
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly.
- Failures are values: nothing panics on bad input.
