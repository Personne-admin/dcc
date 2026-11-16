# A fixed canvas in any window

Players resize windows. Your game should not care. With *logical
presentation* the game draws in a fixed size, here 160 x 90, and SDL scales
the picture to whatever the window is, keeping the proportions with black
bars (`LETTERBOX`) where the shapes do not fit.

`src/canvas.dc` has four placeholders. `src/main.dc` is finished: it opens a
resizable 640 x 480 window, calls `setup`, draws every frame, and prints the
logical position of every mouse click.

## Task 1: setup

Call `SDL_SetRenderLogicalPresentation(renderer, LOGICAL_W, LOGICAL_H, mode)`
with the mode
`sdl3::SDL_RendererLogicalPresentation::SDL_LOGICAL_PRESENTATION_LETTERBOX`.
From now on all drawing calls use *logical* coordinates.

## Task 2: draw

The starter only clears. Add, in logical coordinates:

| Part | Geometry | Color (r, g, b) |
|---|---|---|
| frame | outlined rect x 0, y 0, w 160, h 90 | 240, 190, 60 |
| square | filled rect x 70, y 35, w 20, h 20 | 230, 80, 100 |

The clear color stays (20, 24, 40). The frame hugs the edges of the logical
canvas, whatever the window size is. The square is in the middle.

## Task 3: letterbox

`letterbox(window_w, window_h)` returns the `Mapping` SDL uses:

- `scale` is the largest factor that fits both ways: the smaller of
  `window_w / 160` and `window_h / 90` (as floats).
- The scaled canvas is `160 * scale` by `90 * scale` pixels. The leftover
  space is split evenly: `offset_x = (window_w - 160 * scale) / 2`, and the
  same for y.

For a 320 x 240 window: scale 2, offset (0, 30). The bars are 30 pixels tall,
at the top and the bottom.

## Task 4: to_logical

Mouse positions arrive in *window* coordinates. `to_logical(m, wx, wy)`
undoes the mapping: subtract the offset, then divide by the scale. In a
320 x 240 window, the window point (100, 120) is the logical point (50, 45).

The check compares your function with SDL's own conversion
(`SDL_RenderCoordinatesFromWindow`) at four points, so the two must agree.

## Look at what the harness reads back

With logical presentation, the pixels SDL reads back are the *canvas only*:
for a 320 x 240 window that is a 320 x 180 image, and each logical pixel is
a 2 x 2 block. The frame you draw 1 pixel thick is therefore 2 pixels thick
in the window. `learndc run` shows the same picture with black bars; resize
the window to watch the canvas scale.
