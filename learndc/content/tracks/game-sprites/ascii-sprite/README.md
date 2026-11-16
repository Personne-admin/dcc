# Pixel art in text

A sprite does not need an image file. `src/sprite.dc` holds the hero as eight strings of eight characters, one
character per pixel:

```text
..rrrr..
.rrrrrr.
.#oooo#.
.#o##o#.
..oooo..
.bbbbbb.
.b.bb.b.
..#..#..
```

`create` (already written) turns such art into a texture: it calls your `decode` to fill a byte array, uploads it
and selects nearest-neighbour scaling, exactly like the previous exercise. `draw` copies the texture to the
screen scaled by a factor. You write the two functions in the middle.

## color_for

`color_for(c)` returns the `Rgba` for a character:

| meaning | character | red, green, blue, alpha |
|---|---|---|
| outline | `#` | 30, 30, 50, 255 |
| skin | `o` | 240, 190, 140, 255 |
| red hat | `r` | 200, 40, 40, 255 |
| blue shirt | `b` | 50, 90, 200, 255 |
| transparent | anything else, including `.` | 0, 0, 0, 0 |

Use `match c { 'r' => return ..., _ => return ... }`.

## decode

`decode(art, pixels)` writes every character of every row into `pixels`, four bytes per pixel in the order red,
green, blue, alpha. The width is the length of the first row, and pixel (x, y) starts at byte
`(y * width + x) * 4`. Loop over the rows with a counter for y and over the characters of a row with a counter for x.

## What you should see

The hero, eight times enlarged, in the middle of a dark gray window. Where the art has dots the gray shows
through, because those pixels have alpha 0 and a texture with an alpha channel is blended onto what is behind it.
`learndc run` opens the window; Esc quits.

The first check tests `color_for` and `decode` without any window. The second renders the sprite on an
offscreen window and reads pixels back, which proves the SDL drawing works, not that a window appears on screen.
