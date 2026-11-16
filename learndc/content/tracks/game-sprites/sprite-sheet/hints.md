## Hint 1

A sheet is a grid, and an index counts cells left to right, then down. Turn the index into a column and a row with `%` and `/` by the number of columns.

## Hint 2

`frame_rect`: `col = index % columns`, `row = index / columns`, then `x = col * size`, `y = row * size`. `draw_frame`: take `src` from `frame_rect(index, COLUMNS, FRAME)`, make `dst` with the same size times `scale`, choose the flip mode with an `if`, and call `SDL_RenderTextureRotated` with angle 0.0.

## Hint 3

```dc
sdl3::SDL_FRect src = frame_rect(index, COLUMNS, FRAME);
sdl3::SDL_FRect dst = {x = x, y = y, w = src.w * scale, h = src.h * scale};
sdl3::SDL_FlipMode mode = sdl3::SDL_FlipMode::SDL_FLIP_NONE;
if flip { mode = sdl3::SDL_FlipMode::SDL_FLIP_HORIZONTAL; }
sdl3::SDL_RenderTextureRotated(renderer, sheet, &src, &dst, 0.0, null, mode);
```
