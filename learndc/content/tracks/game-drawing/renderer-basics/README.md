# The renderer: clear, color, shapes, data

A window is a rectangle the operating system owns. To put pixels in it you
need a **renderer**, an SDL object that draws into one window. This lecture
explains how it works and how a DC program talks to it. The exercises that
follow draw real shapes.

## A renderer is a painter with a current color

You never "draw a red square" in one call. A renderer remembers a *draw
color*; every drawing call paints with it:

```dc
sdl3::SDL_SetRenderDrawColor(renderer, 230, 120, 40, 255);   // r, g, b, alpha
sdl3::SDL_FRect box = {x = 40.0, y = 30.0, w = 80.0, h = 60.0};
sdl3::SDL_RenderFillRect(renderer, &box);
```

Each of `r`, `g`, `b` is a `u8` from 0 to 255. The last number is *alpha*,
the opacity; 255 is fully opaque, which is what you want until you teach the
renderer about blending.

## A frame: clear, draw, present

Drawing goes to a hidden *back buffer*, not to the screen. A frame is always
the same three steps:

1. `SDL_RenderClear(renderer)` fills the back buffer with the draw color.
   Without it, last frame's picture is still there and everything smears.
2. Your drawing calls.
3. `SDL_RenderPresent(renderer)` swaps the finished picture onto the window.

The player never sees a half-drawn frame, because it is shown only when
`SDL_RenderPresent` is called. Present once per frame, at the end.

## Coordinates and rectangles

The origin `(0, 0)` is the **top left** corner of the window. x grows to the
right and **y grows downwards**. That is the opposite of a maths graph, and
the source of many "upside down" bugs.

`SDL_FRect` has four `f32` fields: `x` and `y` (the top left corner) and `w`
and `h`. Pixels are whole numbers, but SDL takes floats so that game objects
can sit at positions like `12.5`; the renderer rounds when it draws. The
calls that take a rectangle read it through a pointer, hence `&box`.

- `SDL_RenderFillRect` covers `x` up to but not including `x + w`.
- `SDL_RenderRect` draws only the outline, one pixel thick, inside the same
  box.
- `SDL_RenderLine(renderer, x1, y1, x2, y2)` includes both end points.
- `SDL_RenderPoint(renderer, x, y)` is one pixel.

## Painter's order

Every call paints over whatever is there. The order of the calls is the
layering, exactly as a painter paints the background first. The example
simulates that with a text canvas, so you can see it without a window: the
same two rectangles drawn in two orders.

```dc example=painter_order
```

The top left of the picture is the top left of the window, and the second
rectangle simply hides part of the first.

## Shapes from data

Hard-coding one call per shape does not scale. A game keeps its shapes as
*data* and draws them with one loop. Here a list of heights becomes a bar
chart. Remember y grows downwards: a bar that stands on a baseline has its
top at `baseline - height`.

```dc example=bars_from_data
```

The function takes the values as a slice, `[]const i32`, which accepts any
array by slicing it (`values[0..4]`). The same idea works with an array of
structs, for example a `Shape` with a rectangle, a color and a flag for
"filled". The renderer loop reads each struct and calls the matching
function; the shapes themselves never mention SDL.

## Logical presentation

A game often wants a fixed picture size, say 160 x 90, whatever the window
size is. `SDL_SetRenderLogicalPresentation` tells the renderer to draw in
that fixed coordinate system and scale the result to fit the window.

```dc
sdl3::SDL_SetRenderLogicalPresentation(
    renderer, 160, 90,
    sdl3::SDL_RendererLogicalPresentation::SDL_LOGICAL_PRESENTATION_LETTERBOX);
```

`LETTERBOX` keeps the aspect ratio and adds black bars where the window is
too wide or too tall. (`STRETCH` fills the window and distorts, `INTEGER_SCALE`
only scales by whole numbers.) Underneath is a scale and an offset. In a
320 x 240 window, a 160 x 90 picture is scaled by 2 and moved down 30
pixels:

```dc example=letterbox_math
```

You will need this mapping again for the mouse: the mouse position arrives in
*window* coordinates and your game lives in *logical* ones.

## How the checks look at pixels

The checks of this path render with SDL's `offscreen` video driver, a real
software renderer without a screen, and read the finished frame back with
`SDL_RenderReadPixels`. They then compare a few pixels. This proves that your
calls draw what you claim; it does not prove how the window looks on your
desktop. For that, run the program.

## Common mistakes

- **Forgetting `SDL_RenderPresent`.** The window stays blank or shows the
  old frame.
- **Forgetting the clear.** Moving shapes leave trails.
- **Setting the color after the draw call.** The color applies to the next
  call, not the previous one.
- **Passing the rectangle by value.** SDL wants a pointer: `&box`.
- **y up.** A bar that "grows" from the bottom has its top at
  `baseline - height`, not `baseline + height`.
- **Integer division in float math.** `3 / 2` is 1 in DC; write `3.0 / 2.0`
  or convert with `as f32` before dividing.
