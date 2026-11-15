## Hint 1

`Array(i32)` is created empty with `{}` and must be released with
`deinit()`. The function that creates the array is the one that deinitializes
it; a function that only receives `Array(i32)*` leaves cleanup to its owner.

## Hint 2

`collect_evens` is a `for value in values` loop with an `if` around
`out.push(value)?;`. `print_evens` declares the array, puts
`defer evens.deinit();` on the next line, passes `&evens` to `collect_evens`
(with `?`), then loops over `evens.items()` printing with `println`.

## Hint 3

In `src/numbers.dc`, `collect_evens` gets
`for value in values { if value % 2 == 0 { out.push(value)?; } }` before its
`return`. `print_evens` gets `Array(i32) evens = {};`,
`defer evens.deinit();`, `collect_evens(&evens, values)?;` and a
`for even in evens.items()` loop containing `println("even: {}", even)?;`.
