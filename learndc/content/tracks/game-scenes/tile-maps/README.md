# Tile maps from text

A dungeon is a grid, and a grid is easy to write as text: one character per tile, one line per row. The game turns the
text into numbers once, and then asks the numbers. `src/map.dc` has a 16 by 12 level (`LEVEL`) and the `Map` struct:

```dc
public struct Map {
    u8[192] cells;       // COLS * ROWS tiles, 0 = floor, 1 = wall
    i32 start_col;
    i32 start_row;
}
```

The three characters are `#` for a wall, `.` for floor and `@` for the player's start, which is also floor. Tile
(col, row) is stored at `cells[row * COLS + col]`, the same row-major formula as pixels in a picture.

Edit `src/map.dc` and write three functions.

## parse

`parse(m, rows)` fills `m` from the text and returns `true`. It returns `false` (a bad level must not crash the game) when
there are not exactly `ROWS` rows, a row is not `COLS` characters long, a character is none of `#`, `.`, `@`, or there is no
`@`. Rows are `[]const char`; `rows[row as usize]` is a row and `line[col as usize]` a character.

## is_wall

`is_wall(m, col, row)` tells whether a tile blocks movement. Anything outside the grid counts as wall, so the player can
never walk off the map and later code never has to check bounds itself.

## draw

`draw(renderer, m, cam_x, cam_y)` fills every tile with `fill` (provided), walls (150, 100, 60) and floor (40, 36, 48).
The **camera** is the world position that appears at the top left of the window. A tile at world position `(col * TILE, row * TILE)`
is therefore drawn at `(col * TILE - cam_x, row * TILE - cam_y)`. With a camera of (0, 0) the map fills the window; with
a bigger map and a camera that follows the player, the same code scrolls.

## What you should see

A maze of brown walls on dark floor filling the 320 by 240 window. The arrow keys move the camera: the maze slides in the
opposite direction, and black appears where the map ends. `learndc run` opens the window; Esc quits.

The first two checks parse good and bad maps with no window. The third draws offscreen and reads pixels back; it checks
tile colors and positions, not an on-screen window.
