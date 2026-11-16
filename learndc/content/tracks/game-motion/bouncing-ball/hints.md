## Hint 1

Move first, then look at x and y separately. The two axes are independent, so write the same small block twice.

## Hint 2

For x: `if b.x < 0.0 { b.x = 0.0 - b.x; b.vx = 0.0 - b.vx; bounces += 1; } else if b.x > max_x { ... }` with `max_x = WIDTH - b.size`. Use `else if` so one axis bounces at most once per step.

## Hint 3

```
} else if b.x > max_x {
    b.x = max_x - (b.x - max_x);
    b.vx = 0.0 - b.vx;
    bounces += 1;
}
```
