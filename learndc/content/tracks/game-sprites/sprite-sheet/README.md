# Sprite sheets

Four animation frames or four different sprites rarely deserve four textures. A **sprite sheet** is one texture that
holds all of them in a grid, and drawing frame number `n` means copying only that part of the texture. `src/sheet.dc`
contains a 16 by 16 sheet of four 8 by 8 frames, decoded and uploaded for you (`create`). Each frame has a dark outline
and a white marker pixel at its top left, and a fill color: red, green, blue, yellow.

```text
+----------+----------+
| 0 red    | 1 green  |      COLUMNS = 2, FRAME = 8
+----------+----------+
| 2 blue   | 3 yellow |
+----------+----------+
```

## frame_rect

`frame_rect(index, columns, size)` returns the source rectangle of a frame: the column is `index % columns`, the row is
`index / columns`, the position is column and row times `size`, and the width and height are `size`. It must work for
any number of columns, not only two.

## draw_frame

`draw_frame(renderer, sheet, index, x, y, scale, flip)` draws one frame with its top left corner at (x, y), enlarged
by `scale`:

- the source is `frame_rect(index, COLUMNS, FRAME)`;
- the destination has the source's size times `scale`;
- `SDL_RenderTexture` has no mirroring, so use `SDL_RenderTextureRotated(renderer, sheet, &src, &dst, 0.0, null, mode)`.
  The `0.0` is the rotation angle in degrees, `null` rotates around the center, and `mode` is
  `SDL_FlipMode::SDL_FLIP_HORIZONTAL` when `flip` is true and `SDL_FlipMode::SDL_FLIP_NONE` otherwise. A character
  that walks left is the same sprite drawn mirrored; you need no second set of frames.

Edit `src/sheet.dc`.

## What you should see

The window shows the four frames in a row, and the same four mirrored below them: in the mirrored row the white
marker is at the top right. `learndc run` opens the window; Esc quits.

The first check tests `frame_rect` with no window. The second renders on an offscreen window with SDL's software
renderer and reads pixels back; it checks drawing, not an on-screen window.
