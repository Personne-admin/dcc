# Why the solution works

```dc
public i32 area(Shape s) {
    return match s {
        Shape::Circle(radius) => 3 * radius * radius,
        Shape::Rect(size) => size.width * size.height,
        Shape::Empty => 0,
    };
}
```

- A `Shape` stores which variant it is together with that variant's data. The
  `match` checks the variant and, in the matching arm only, makes the data
  available under the name in the pattern: `radius` is an `i32` in the first
  arm and `size` is a `Size` in the second. There is no way to read the
  radius out of a `Rect`, so a whole class of mix-ups cannot happen.
- Because `match` is an expression, each arm produces an `i32` and the whole
  `match` is returned.
- The compiler requires every variant to be covered. That is what rejected
  the starter, and it is also the reason it is safe to extend `Shape` later:
  each `match` that has not been updated stops compiling instead of silently
  falling through.
- Variants are written with their enum name, `Shape::Rect(...)`, both when
  constructing a value and when matching on one. `Shape::Empty` has no data,
  so it has no parentheses.
