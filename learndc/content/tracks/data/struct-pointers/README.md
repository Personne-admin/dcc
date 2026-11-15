# Structs

A struct groups related values under one name. DC structs hold data only:
behavior lives in ordinary functions next to them.

```dc
public struct Rect {
    i32 width;
    i32 height;
}

public i32 area(Rect r) {
    return r.width * r.height;
}

Rect hall = Rect { width = 4, height = 3 };   // struct literal
println("{}", hall.area())?;                  // same as area(hall)
```

- A struct literal names the fields: `Rect { width = 4, height = 3 }`.
- If a function's first parameter is a `Rect`, you can call it with dot
  syntax, `hall.area()`. This is called UFCS and is only a nicer spelling of
  `geometry::area(hall)`.
- A `Rect` parameter receives a copy. To change the caller's value, take a
  pointer, `Rect* r`. Dot syntax passes the address for you (`hall.grow(2)`),
  and `r.width` reaches through the pointer.

## Your task

Open `src/geometry.dc`. The struct is declared, but its three functions are
stubs. Implement them:

- `area(Rect r)` returns width times height.
- `grow(Rect* r, i32 amount)` adds `amount` to both the width and the height
  of the caller's rectangle.
- `square(i32 side)` returns a `Rect` whose width and height are both `side`.

## Acceptance criteria

- The project builds with `dpm build`.
- Running it prints exactly:

```text
hall: 4x3, area 12
grown: 6x5, area 30
cell: 5x5, area 25
```
