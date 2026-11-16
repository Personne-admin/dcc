# Root cause: Shipped anyway

`validate(order);` is a statement that evaluates a call and discards its result. The compiler accepts that for `Status` values too, so the failure was silently dropped and `ship` always went on to print the shipped line. A function that can fail is only as safe as its caller's handling of the result.

The method: when "the check exists but nothing happens", follow the verdict from the place it is produced to the place it should be used, and look for a call used as a statement. Adding `?` would pass the error up, but `ship` returns `Status(Error)` and the types differ, besides the requirement is to print a reason, so `match` is the right tool.
