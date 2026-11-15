# Passing errors upward with ?

Matching every result by hand gets long. The postfix `?` does the common
thing for you: when the operation succeeded it continues (and for a `Result`
it gives you the success value); when it failed it returns the error from
the current function at once.

```dc
Result(usize, Error) reserve(usize left, usize amount) {
    if amount > left {
        return Result::Err(Error::Allocation(AllocError::OutOfMemory));
    }
    return Result::Ok(left - amount);
}

Status(Error) plan() {
    usize left = 100;
    left = reserve(left, 30)?;
    println("{} left", left)?;
    return Status::Ok;
}
```

`Status(Error)` is a result without a value: `Status::Ok` (no parentheses) or
an error. `Error` is the standard error type; printing, parsing and
allocation failures all convert into it through `?`.

A hosted `main` may itself return `Status(Error)`. Success exits with code 0.
An error exits with code 1, and nothing is printed for it automatically.

## Your task

Open `src/main.dc`. `reserve` is finished: it takes `amount` units out of
`left` or fails. Complete the program so that:

1. `plan` reserves 30, then 50, then 40 units from the 100 it starts with,
   storing each new total back in `left` and printing
   `reserved N, X left` after each success, and
2. `main` calls `plan()` and passes its failure on.

The third reservation does not fit, so the program must print

```text
start: 100 units
reserved 30, 70 left
reserved 50, 20 left
```

and then exit with code 1, without printing a third `reserved` line.

## Acceptance criteria

- The project builds with `dpm build`.
- The output is exactly the three lines above.
- The program exits with code 1 because the last reservation fails.
