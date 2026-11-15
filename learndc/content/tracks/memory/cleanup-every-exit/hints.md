## Hint 1

Two of the three calls in `main` take an early `return`. Look at what they
print last.

## Hint 2

`defer println("till closed");` directly below `println("till opened")?;`
runs on every return from `checkout`.

## Hint 3

Add the `defer` line and remove the `println("till closed")?;` near the end of
`checkout`; otherwise the last checkout prints the message twice.
