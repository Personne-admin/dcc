# Success or a reason: Result

`Optional` says "no answer" but not why. `Result(T, E)` carries either a
success value, `Result::Ok(value)`, or a reason for failure,
`Result::Err(error)`. The reason can be any type; a small enum is ideal:

```dc
public enum ParseError {
    Empty,
    TooLong,
}

public Result(usize, ParseError) length_of([]const char text) {
    if text.len == 0 { return Result::Err(ParseError::Empty); }
    return Result::Ok(text.len);
}
```

The caller must look at both outcomes. A `match` can be nested: the outer one
picks `Ok` or `Err`, and a second one inside the `Err` arm picks the kind of
error.

```dc
match length_of(name) {
    Result::Ok(n) => println("{} characters", n)?,
    Result::Err(reason) => match reason {
        ParseError::Empty => println("empty")?,
        ParseError::TooLong => println("too long")?,
    },
}
```

## Your task

An age is valid from 0 to 150 inclusive. The enum `AgeError` is already
declared in `src/ages.dc`.

1. Make `check_age` return `Err(AgeError::Negative)` below 0,
   `Err(AgeError::TooOld)` above 150, and `Ok(age)` otherwise.
2. In `src/main.dc`, make `report` match on `checked` and print:

```text
30 is a valid age
-4: ages cannot be negative
200: too old to be real
150 is a valid age
```

## Acceptance criteria

- The project builds with `dpm build`.
- The boundaries 0 and 150 are valid; -1 and 151 are errors of the right kind.
- The program prints the four lines above and exits with code 0.

Run `learndc check` when you are ready, or `learndc run` to see the output.
