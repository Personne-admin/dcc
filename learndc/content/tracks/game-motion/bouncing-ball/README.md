# A ball that bounces off four walls

The ball lives in a 320 x 240 box. `src/ball.dc` has the `Ball` struct (the
top left corner `x`, `y`, the velocity `vx`, `vy` in pixels per second and
the `size` of the square ball) and a placeholder `step`. `src/main.dc`
is finished: it moves the ball with the real frame time, draws it, and
shows one yellow dot per bounce at the top.

## Task: step(ball, dt) returns the number of bounces

1. **Move**: `x += vx * dt`, `y += vy * dt`.
2. **Test each axis** against its limits. The ball's position is its top left
   corner, so the allowed range is `0.0 <= x <= WIDTH - size` and
   `0.0 <= y <= HEIGHT - size`.
3. **Reflect** when a limit is crossed:
   - set the velocity component to its negative (only that axis; the other
     one keeps its direction),
   - move the position back by the overshoot, *mirroring* it at the wall:
     past the right limit, `x = max_x - (x - max_x)`; past the left limit,
     `x = -x`.
4. Count one bounce for each axis that was reflected, and return the total:
   0, 1 or 2 (a corner counts 2).

```text
ball x = 295, vx = 100, size 10, dt = 0.2  (limit max_x = 310)
move:    x = 315            5 past the limit
mirror:  x = 310 - 5 = 305  vx = -100       1 bounce
```

Why mirror instead of setting `x = max_x`? The ball travelled the whole
distance in this step; the part past the wall is spent going back. Mirroring
keeps the motion natural and also makes sure that the ball is *inside* the
box again, so it cannot bounce twice for the same wall. Touching a limit
exactly is not a bounce.

## The second check

It runs 3000 steps of 1/60 s and requires the ball to be inside the box after
each one, to have bounced many times and to have kept its speed.

## Run it

`learndc run`. Each bounce adds a yellow dot.
