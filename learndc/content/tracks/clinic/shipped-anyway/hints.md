## Hint 1

Run the validator alone on a bad order to see where the verdict is lost.

## Hint 2

The result of `validate(order)` is never looked at.

## Hint 3

Match on `validate(order)`; for `Status::Err(why)` match `why` on `OrderError::NoItems` and `OrderError::NoAddress`.
