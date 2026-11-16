# A house from rectangles, lines and points

In the last lecture you saw that a renderer draws with a *current color*:
you set the color, then call a drawing function, and everything after the
call keeps using that color until you change it. Now use it. `src/scene.dc`
has `draw(renderer)`, which today clears the window to black. `set_color` is
a helper that sets the color with full opacity. `src/main.dc` is a finished
program: it calls `draw` every frame and shows the result with
`SDL_RenderPresent`.

The window is 200 x 150 pixels. The origin `(0, 0)` is the **top left**
corner, x grows to the right and **y grows downwards**.

## The drawing calls

| Call | Draws |
|---|---|
| `SDL_RenderClear(renderer)` | the whole frame in the current color |
| `SDL_RenderFillRect(renderer, &rect)` | a filled rectangle |
| `SDL_RenderRect(renderer, &rect)` | only the border of a rectangle, one pixel thick |
| `SDL_RenderLine(renderer, x1, y1, x2, y2)` | a line, both end points included |
| `SDL_RenderPoint(renderer, x, y)` | one pixel |

Rectangles are `SDL_FRect` structs with `f32` fields:
`SDL_FRect box = {x = 60.0, y = 60.0, w = 80.0, h = 50.0};` and are passed by
address (`&box`) because SDL reads them through a pointer. A filled rectangle
covers `x` up to but not including `x + w`; an outline sits inside the same
box.

## The scene

Draw in this order. Later shapes cover earlier ones (*painter's order*).

| Part | Call | Geometry | Color (r, g, b) |
|---|---|---|---|
| sky | clear | whole frame | 110, 170, 230 |
| ground | filled rect | x 0, y 110, w 200, h 40 | 60, 150, 70 |
| house body | filled rect | x 60, y 60, w 80, h 50 | 200, 170, 120 |
| door | filled rect | x 90, y 80, w 20, h 30 | 110, 70, 40 |
| window frame | outlined rect | x 68, y 68, w 16, h 14 | 255, 255, 255 |
| roof | two lines | (55, 60) to (100, 15), then (100, 15) to (145, 60) | 180, 50, 50 |
| three birds | three points | (20, 20), (30, 25), (40, 20) | 20, 20, 20 |

The door's bottom edge lies on the ground's top edge (`110`), and the window
frame is only an outline, so the wall color shows through it.

## See it

`learndc run` opens a 200 x 150 window; Esc or closing the window quits. The
checks render the same function with SDL's `offscreen` video driver and read
pixels back, so they test SDL's real rendering calls but not a visible
window.

An optional polish idea for later: add a chimney, a second window or a sun.
No check looks at it.
