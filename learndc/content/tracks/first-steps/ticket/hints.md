## Hint 1

Every line of the ticket is produced by `println` or by a `print` that a
`println` finishes. Work from the top and check each stage as you go.

## Hint 2

Quote text arguments and leave numbers bare. The farewell line is three calls
in a row: `print` the greeting, `print` the name through `{}`, and `println`
the exclamation mark.

## Hint 3

The heading is `println("==== TRAIN TICKET ====")?;` and the farewell is
`print("Have a good trip, ")?;`, `print("{}", "Ada")?;` and `println("!")?;`.
The stage hints show the other lines.
