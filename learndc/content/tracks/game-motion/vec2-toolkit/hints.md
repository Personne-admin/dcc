## Hint 1

Four of the operations are one-line struct literals. `length`, `normalized` and `integrate` reuse them.

## Hint 2

`length`: `std::math::sqrt((v.x * v.x + v.y * v.y) as f64) as f32`. `normalized`: compute the length, return the zero vector when it is 0.0, otherwise divide both components by it. `integrate`: two assignments, in the order velocity, position.

## Hint 3

```
body.velocity = add(body.velocity, scale(acceleration, dt));
body.position = add(body.position, scale(body.velocity, dt));
```
