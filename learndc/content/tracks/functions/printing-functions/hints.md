## Hint 1

A function that prints returns `Status(Error)`. Every `println(...)` or call to
another such function gets a `?` after it, and the last line is
`return Status::Ok;`.

## Hint 2

`print_banner` and `print_item` each need a single `println` with `{}`
placeholders. `print_receipt` calls `print_banner("Corner Shop")?;`, then
`print_item` for bread and milk, then once more for the total, which is
`bread + milk`.

## Hint 3

`println("== {} ==", text)?;` and `println("{}: {}", name, price)?;`. In
`print_receipt`: `print_banner("Corner Shop")?;`, `print_item("bread", bread)?;`,
`print_item("milk", milk)?;` and `print_item("total", bread + milk)?;`.
