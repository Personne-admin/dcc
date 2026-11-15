## Hint 1

Inside `area` the parameter `r` is a `Rect`, so its fields are `r.width` and
`r.height`. Inside `grow` the parameter is a `Rect*`, and the same `r.width`
syntax reads and writes the caller's field through the pointer.

## Hint 2

`area` is a single `return` of a product. `grow` changes two fields with `+=`
and returns nothing. `square` must build a new `Rect` with a struct literal
that uses the same `side` for both fields.

## Hint 3

In `src/geometry.dc`: `area` returns `r.width * r.height;`. `grow` has the two
statements `r.width += amount;` and `r.height += amount;`. `square` returns
`Rect { width = side, height = side }`.
