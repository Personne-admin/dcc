## Hint 1

The error says the `match` is not exhaustive and names the missing case.
Every variant of the enum needs an arm, even if the arm does nothing
interesting.

## Hint 2

An arm for a variant with data looks like the `Circle` arm: the pattern
`Shape::Circle(radius)` gives the name `radius` to the payload for use on the
right of `=>`. The `Rect` payload is a `Size`, so the name you bind has the
fields `width` and `height`.

## Hint 3

In `area` in `src/shapes.dc`, add one line between the `Circle` and `Empty`
arms: `Shape::Rect(size) => size.width * size.height,`.
