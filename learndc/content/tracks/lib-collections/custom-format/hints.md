## Hint 1

Copy the shape of the `Span` example from this page: the signature is already in your file, only the body is missing.

## Hint 2

`Money`: `i64 magnitude = if self.cents < 0 { 0 - self.cents } else { self.cents };` then `out.write_all("-")?;` when the amount is negative, then one `std::fmt::format(out, "${}.{:02}", magnitude / 100, magnitude % 100)?;`.

## Hint 3

`Level`: `match *self { Level::Info => std::fmt::write_padded(out, "INFO", opts)?, Level::Warn => ..., Level::Fail => ... }` followed by `return Status::Ok;`. For `Span` end with `return std::fmt::format(out, "{}..{}", self.low, self.high);`.
