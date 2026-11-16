## Hint 1

`overlaps` is one boolean expression. `resolve` is: return early if no overlap, measure two depths, then one `if` on the smaller depth with two branches each (which side).

## Hint 2

Helpers are worth writing: `smaller(a, b)`, `larger(a, b)` and `magnitude(v)` (absolute value). With them `depth_x = smaller(m.rect.x + m.rect.w, wall.x + wall.w) - larger(m.rect.x, wall.x)`. If `depth_x < depth_y` correct x, otherwise y.

## Hint 3

```
if mover_center < wall_center {
    m.rect.x -= depth_x;
    m.vx = 0.0 - magnitude(m.vx);
} else {
    m.rect.x += depth_x;
    m.vx = magnitude(m.vx);
}
```
