# Tagged enums

A tagged enum is a value that is exactly one of several named variants, and a
variant can carry data. It is how DC says "this is either a circle with a
radius, or a rectangle with a size, or nothing at all".

```dc
public enum Shape {
    Circle(i32),      // carries a radius
    Rect(Size),       // carries a Size struct
    Empty,            // carries nothing
}

Shape s = Shape::Circle(2);
Shape e = Shape::Empty;      // no parentheses for a variant without data
```

A variant carries at most one value; to carry several, wrap them in a struct
as `Rect(Size)` does.

`match` looks at which variant a value is and binds its data to a name:

```dc
i32 corners(Shape s) {
    return match s {
        Shape::Circle(_) => 0,
        Shape::Rect(_) => 4,
        Shape::Empty => 0,
    };
}
```

`_` ignores the data. A `match` must cover every variant, so adding a variant
later makes the compiler point at every `match` that needs updating.

## Your task

The starter does not build: `area` in `src/shapes.dc` forgets the `Rect`
variant. Read the compiler's message, then add the missing arm. A rectangle's
area is its width times its height; bind the payload to a name (such as
`size`) to read its fields.

## Acceptance criteria

- The project builds with `dpm build`.
- Running it prints exactly:

```text
disc: area 12, corners 0
wall: area 15, corners 4
nothing: area 0, corners 0
```

Run `learndc check` when you are ready, or `learndc run` to see the output.
