## Hint 1

A box covers a rectangle of tiles. Turn its corners into tile coordinates, then test every tile between them. Move one axis, resolve, then the other axis.

## Hint 2

For `blocked`: first column is `x / tile`, last is `(x + w - 0.001) / tile`, same for rows; guard negative values first. For `move`: `nx = b.x + dx`; if `blocked(m, nx, b.y, ...)` snap `nx` flush to the wall (right edge: `col * tile - w`, left edge: `(col + 1) * tile`), then store it and repeat for y with the new x.

## Hint 3

```dc
if blocked(m, nx, b.y, b.w, b.h) {
    if dx > 0.0 {
        nx = ((((nx + b.w) / tile) as i32) as f32) * tile - b.w;
    } else {
        nx = ((((nx / tile) as i32) + 1) as f32) * tile;
    }
}
b.x = nx;
```
