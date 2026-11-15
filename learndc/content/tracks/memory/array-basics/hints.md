## Hint 1

The first two lines of `main` are `Array(i32) squares = {};` and
`defer squares.deinit();`, exactly as in the lecture.

## Hint 2

Inside `for i32 n in 1..=5 { ... }` write `squares.push(n * n)?;`. Without the
`i32`, `n` would be a `usize` and the compiler would find no `push` for it. Then loop over
`squares.items()` to print and to add up.

## Hint 3

```dc
i32 sum = 0;
for square in squares.items() {
    println("{}", square)?;
    sum += square;
}
println("{} squares, sum {}", squares.len(), sum)?;
```
