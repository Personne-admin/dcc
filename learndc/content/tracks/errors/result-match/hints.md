## Hint 1

A `Result(i32, AgeError)` is built with `Result::Ok(some_i32)` or
`Result::Err(some_age_error)`, and an enum value is written
`AgeError::Negative`. Reading it back takes a `match` with an arm for `Ok` and
an arm for `Err`.

## Hint 2

In `check_age`, handle the two error ranges with early `if` returns and finish
with the `Ok`. In `report`, the `Err` arm receives an `AgeError`, so give it a
second `match` with one arm per variant of `AgeError`. Every arm prints with
`println(...)?`.

## Hint 3

In `src/ages.dc`, `if age < 0 { return Result::Err(AgeError::Negative); }`,
the same for `age > 150` with `TooOld`, then `return Result::Ok(age);`. In
`src/main.dc`, replace the `println` in `report` with
`match checked { Result::Ok(valid) => ..., Result::Err(problem) => match
problem { ages::AgeError::Negative => ..., ages::AgeError::TooOld => ... }, }`;
the enum is reached as `ages::AgeError` from the main module.
