# Rectangle overlap and push-out

Walls, paddles, crates and the player are all rectangles whose sides are
parallel to the axes. Two things are needed: *did they collide?* and *what
now?* `src/boxes.dc` has the `Rect` and `Mover` structs (a rectangle with a
velocity) and two placeholders. `src/main.dc` is a finished toy: steer a
square with the arrows or WASD in a room with a block in the middle; it can
not enter any of them.

## Task 1: overlaps(a, b)

`a` and `b` overlap when their x ranges overlap *and* their y ranges overlap.
Ranges `[a.x, a.x + a.w)` and `[b.x, b.x + b.w)` overlap when each starts
before the other ends:

```text
a.x < b.x + b.w  &&  b.x < a.x + a.w      (x ranges)
a.y < b.y + b.h  &&  b.y < a.y + a.h      (y ranges)
```

All four with `&&`. Use `<` and not `<=`: two rectangles that only touch along
an edge, like a ball resting on the floor, are not colliding.

## Task 2: resolve(mover, wall)

`resolve` returns `false` and changes nothing when the mover does not overlap
the wall. When it does:

1. **Depth on each axis**: how far the rectangles overlap.
   `depth_x = min(a.right, b.right) - max(a.left, b.left)`, where
   `right = x + w`; the same for y with `bottom = y + h`.
2. **Pick the axis with the smaller depth.** The mover entered through that
   side: shallow penetration means it only just crossed that edge.
3. **Push out along that axis, toward the side it came from.** Compare the
   centers: if the mover's center is left of the wall's center, move it left
   by `depth_x` (`x -= depth_x`), otherwise right (`x += depth_x`). The same
   with y: above means up, below means down.
4. **Set the velocity on that axis to point away from the wall**: if pushed
   left, `vx = -|vx|`; if pushed right, `vx = |vx|`. The other axis keeps its
   velocity. (This is not a blind negation: a mover that is already moving
   away keeps going away.)
5. Return `true`.

```text
mover x 0..10 moving right at 50, wall x 8..18:
depth_x = 2, depth_y = 10   ->  x -= 2   ->  x = -2, vx = -50
```

There is no helper for `min`, `max` or the absolute value in the starter: write
small functions, or use `if`.

## Run it

`learndc run` and walk into the walls: you slide along them because only
the penetrating axis is corrected. The checks cover each side of a wall, no
contact, touching edges and a mover that is already leaving.
