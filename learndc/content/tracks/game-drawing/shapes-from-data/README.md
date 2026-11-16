# Shapes from data

A game that draws a hundred things does not contain a hundred draw calls.
It holds the things as *data*, a list of structs, and one loop turns each
into a draw call. `src/chart.dc` defines the data and two functions to
write. `src/main.dc` is finished: it builds a list of shapes and a list of
sales figures and draws both every frame.

```dc
public struct Color {
    u8 r;
    u8 g;
    u8 b;
}

public struct Shape {
    sdl3::SDL_FRect rect;
    Color color;
    bool filled;
}
```

`set_color(renderer, color)` is given; it sets the draw color from a `Color`.

## Task 1: draw_shapes

`draw_shapes(renderer, shapes)` receives a slice, `[]const Shape`. For every
shape, in list order:

1. set the color from `shape.color`,
2. draw `shape.rect` filled when `shape.filled` is true and as an outline
   otherwise.

`for shape in shapes { ... }` gives you each element. The rectangle functions
want a pointer, and `&shape.rect` is the address of the field.

Because the shapes are drawn in list order, a later shape covers an earlier
one: the order in the data is the layering.

## Task 2: draw_bars

`draw_bars(renderer, values, bar_width, gap, baseline, color)` draws one bar
per number. Bar `i` is `bar_width` wide, `value` tall, starts at
`x = i * (bar_width + gap)` and **stands on** the line `y = baseline`.

```text
values {30, 60, 45}, bar_width 20, gap 10, baseline 100

  bar 0: x 0..19,  y 70..99     (top = 100 - 30)
  bar 1: x 30..49, y 40..99     (top = 100 - 60)
  bar 2: x 60..79, y 55..99     (top = 100 - 45)
```

y grows downwards, so the top of a bar is `baseline - value`. The numbers are
`i32` but `SDL_FRect` holds `f32`: convert each field with `as f32`
(`x as f32`). Set the color once before the loop.

## See it

`learndc run` shows three overlapping shapes and six green bars. Esc or
closing the window quits. Try changing the numbers in `main.dc`: the picture
follows the data. The checks draw their own data into an offscreen renderer
and compare pixels.
