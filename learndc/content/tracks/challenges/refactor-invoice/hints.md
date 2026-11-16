## Hint 1

First write the module with the struct and the two functions, and check them with the harness. Then change `main`.

## Hint 2

The three blocks in `main` differ only in the name, quantity and unit price: those are the fields of `Line`. `line_total` is one of those blocks as a function.

## Hint 3

In `main`: `Line[3] lines = { Line { name = "widget", qty = 3, unit = 250 }, ... };`, a `for line in lines` loop printing `line_total(line)`, then `invoice_total(lines)`. Pass the array where a `[]const Line` is expected.
