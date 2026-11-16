# Tile collision

A player walking through a dungeon is a box that must not enter wall tiles. The map already knows which tiles are walls
(`map.dc` is finished this time). Your job in `src/collision.dc`: ask which tiles a box covers, and move a box without
ever leaving it inside a wall.

```dc
public struct Body {
    f32 x;       // top left corner, in world pixels
    f32 y;
    f32 w;
    f32 h;
}
```

## blocked

`blocked(m, x, y, w, h)` is `true` when a box at that position overlaps a wall tile. A tile is `TILE` pixels wide
(20), so the box covers the columns from `x / TILE` to `(x + w) / TILE` and the rows from `y / TILE` to `(y + h) / TILE`.
Check every tile in that rectangle with `map::is_wall`.

Two details decide whether it is right:

- A box whose right edge is *exactly* on a tile boundary does not overlap the next tile. Use `x + w - 0.001` for the
  last column and `y + h - 0.001` for the last row.
- Truncating a negative number toward zero turns -3 into 0, which is not the tile at -1. Positions left of or above the
  map must still count as wall, so handle negative coordinates before you divide (`is_wall` answers `true` for column -1).

## move

`move(m, b, dx, dy)` moves the body by `dx`, `dy` (small, always less than one tile) in two steps: first the x axis,
then the y axis. Moving the axes separately is what lets the player **slide** along a wall when pushing diagonally into it:
x is refused, y still moves.

For each axis: compute the new position, ask `blocked`, and if it is blocked place the box flush against the wall it hit
instead of leaving it where it was. Flush matters: if you only undo the move, the player stops up to a whole step short of
the wall, and the gap grows with the speed. Moving right, the new right edge is inside tile column `c = (nx + w) / TILE`,
so `x = c * TILE - w`. Moving left, the new left edge is inside column `c = nx / TILE`, so `x = (c + 1) * TILE`. Rows are the same.

## What you should see

The maze with an orange square that starts in the top left corridor. Arrow keys move it at 90 pixels per second, it stops
flush against walls and slides along them, and it cannot leave the map. `learndc run` opens the window; Esc quits.

Both checks are headless: they place boxes on the real level and walk them with scripted steps. They do not test how the
movement feels with a real keyboard.
