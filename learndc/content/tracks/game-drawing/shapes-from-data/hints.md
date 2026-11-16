## Hint 1

Both tasks are a loop over a slice. Inside the loop you set a color and call one drawing function with a rectangle.

## Hint 2

`draw_shapes`: `for shape in shapes { set_color(renderer, shape.color); if shape.filled { ... } else { ... } }`. `draw_bars`: keep an `i32 x` that starts at 0 and grows by `bar_width + gap` after each bar; build an `SDL_FRect` with `y = (baseline - value) as f32` and `h = value as f32`.

## Hint 3

```
sdl3::SDL_FRect bar = {
    x = x as f32,
    y = (baseline - value) as f32,
    w = bar_width as f32,
    h = value as f32,
};
sdl3::SDL_RenderFillRect(renderer, &bar);
```
