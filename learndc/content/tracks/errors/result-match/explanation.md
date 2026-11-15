# Why the solution works

```dc
public Result(i32, AgeError) check_age(i32 age) {
    if age < 0 {
        return Result::Err(AgeError::Negative);
    }
    if age > 150 {
        return Result::Err(AgeError::TooOld);
    }
    return Result::Ok(age);
}
```

- The error cases return early, so reaching the last line means the age is
  in range. The comparisons are strict (`<` and `>`), which keeps 0 and 150
  valid.
- `Result::Err(AgeError::Negative)` builds the failure explicitly: the
  `Err` variant holds an `AgeError`, and the enum tells the caller what went
  wrong instead of a bare "no".

```dc
match checked {
    Result::Ok(valid) => println("{} is a valid age", valid)?,
    Result::Err(problem) => match problem {
        ages::AgeError::Negative => println("{}: ages cannot be negative", age)?,
        ages::AgeError::TooOld => println("{}: too old to be real", age)?,
    },
}
```

- The outer `match` separates success from failure and must cover both
  variants.
- The inner `match` runs only for failures and must cover every variant of
  `AgeError`. Adding a new variant later makes this code stop compiling until
  the new case is handled, which is the point of using an enum for errors.
- `valid` and `problem` are names bound by the patterns; they exist only in
  their own arm.
