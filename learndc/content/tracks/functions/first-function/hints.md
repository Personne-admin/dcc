## Hint 1

A function body is a list of statements that ends with `return value;`. The
value must have the type written in front of the function name. Parameters are
ordinary variables inside the body.

## Hint 2

`triple` returns its parameter times 3 and `rectangle_area` returns the two
parameters multiplied. For `seconds_in`, copy the shape of `twice`: return
type, name, one `i32` parameter named `minutes`, then a body. In `main`, copy
the line above it and change the text and the call.

## Hint 3

In `src/basics.dc`: `return n * 3;`, `return width * height;` and a new
function `public i32 seconds_in(i32 minutes) { return minutes * 60; }`. In
`main`: `println("3 minutes = {} seconds", seconds_in(3))?;`.
