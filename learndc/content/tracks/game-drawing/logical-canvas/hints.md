## Hint 1

setup is one call. draw is the shapes-by-hand pattern in logical coordinates. letterbox and to_logical are arithmetic on f32 values; convert the i32 arguments with `as f32` first.

## Hint 2

`scale = if sx < sy { sx } else { sy }` picks the smaller factor. The used size is `LOGICAL_W as f32 * scale` by `LOGICAL_H as f32 * scale`; half of what is left over is each offset. to_logical is `(window - offset) / scale` for x and y.

## Hint 3

```
sdl3::SDL_SetRenderLogicalPresentation(renderer, LOGICAL_W, LOGICAL_H,
    sdl3::SDL_RendererLogicalPresentation::SDL_LOGICAL_PRESENTATION_LETTERBOX);
```

```
f32 sx = (window_w as f32) / (LOGICAL_W as f32);
f32 sy = (window_h as f32) / (LOGICAL_H as f32);
f32 scale = if sx < sy { sx } else { sy };
```
