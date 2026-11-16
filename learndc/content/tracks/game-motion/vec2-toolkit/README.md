# A Vec2 toolkit

Almost everything in a game that moves is a pair of numbers. Instead of
passing `x` and `y` around separately, you give the pair a name and write the
few operations you need once. `src/vec2.dc` has the `Vec2` and `Body`
structs and placeholders for the operations; `src/main.dc` is a finished toy
that uses them: a ball falls under gravity, and a click throws it toward the
mouse at a fixed speed.

```dc
public struct Vec2 { f32 x; f32 y; }
public struct Body { Vec2 position; Vec2 velocity; }
```

## Task 1: the arithmetic

- `add(a, b)` and `sub(a, b)`: component by component.
- `scale(v, k)`: multiply both components by `k`.
- `dot(a, b)`: `a.x * b.x + a.y * b.y`, one number. It is 0 for perpendicular
  vectors and positive when the two point roughly the same way.

## Task 2: length and direction

- `length(v)` is `sqrt(x * x + y * y)`. `std::math::sqrt` takes and returns
  an `f64`, so write `std::math::sqrt((...) as f64) as f32`.
- `normalized(v)` is the same direction with length 1: divide both
  components by the length. The zero vector has no direction; return the zero
  vector instead of dividing by 0 (which would give `NaN`, a "not a number"
  that poisons everything it touches).

The click-to-throw in `main.dc` is `scale(normalized(sub(target, ball)), 450.0)`:
the direction to the mouse, at exactly 450 pixels per second, from any
distance.

## Task 3: one physics step

`integrate(body, acceleration, dt)` changes the velocity first and then the
position with the *new* velocity:

```text
velocity = velocity + acceleration * dt
position = position + velocity * dt
```

Use your own `add` and `scale`. Gravity is an acceleration like (0, 500): y
grows downwards, so a positive y acceleration falls.

## Run it

`learndc run`: watch the ball fall and bounce, click to throw it. The checks
are pure arithmetic on `Vec2` values; no SDL is initialized at all.
