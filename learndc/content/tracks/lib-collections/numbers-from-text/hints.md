## Hint 1

Handle the three easy answers first: empty text, then the sign. `text[0] == '-' as char` tests a character; `cursor = 1;` skips it.

## Hint 2

After `accumulate_digits`, a `NumError::OutOfRange` is `TooBig` and every other error is `NotANumber`. Then check `cursor != text.len` (leftover characters). Finally call `signed_from_magnitude(magnitude.unwrap(), negative)`; its error is `TooBig` as well.

## Hint 3

`sum_line`: `i64 total = 0;`, loop with `match parts.next()`. In the `Some(token)` arm: `if token.len > 0 { total += parse_i64(token)?; }`. Finish with `return Result::Ok(total);`.
