# Vectors, motion and collisions

Games are about things that move and things that hit each other. This
lecture gives you the small amount of maths behind both, as ordinary DC
structs and functions, with no window involved: every example prints numbers.

## Motion is repeated change

A moving object has a **position**, a **velocity** (how the position changes
per second) and sometimes an **acceleration** (how the velocity changes per
second). Every frame, with `dt` seconds elapsed:

```text
velocity = velocity + acceleration * dt
position = position + velocity * dt
```

That is all of physics for a 2D arcade game. Gravity is just an acceleration
that points down; a paddle that moves at constant speed has no acceleration
at all. Updating the velocity first and the position second is called
*semi-implicit Euler* and is stable enough for games.

## Vectors are small structs

Position and velocity have an x and a y part. Bundle them in a struct and
write the few operations you need as functions:

```dc example=falling
```

`Vec2` has two `f32` fields, and `add` and `scale` return a new one. The
ball starts with a velocity to the right, gravity pulls down (y grows
downwards, so a positive y acceleration is a fall), and the printed position
curves like a thrown ball. Nothing here knows SDL; the same code runs in a
test.

## Length and direction

The **length** of a vector is the Pythagorean theorem,
`sqrt(x * x + y * y)`. Dividing a vector by its length gives a *normalized*
vector: same direction, length 1. Multiply that by a speed and you move at
exactly that speed in any direction, which is also how you fix diagonal
movement from the previous chapter. Take care of the zero vector: dividing
by a zero length would give nonsense, so return zero instead.

`std::math::sqrt` works on `f64`, so the code converts with `as f64` and
back with `as f32`:

```dc example=length
```

## Bouncing off walls

A ball that hits a wall keeps its speed and flips the velocity component
that points into the wall. When a step ends *beyond* the wall, also move the
ball back by the amount it overshot (mirror it), otherwise it would sit
inside the wall for a frame:

```dc example=bounce
```

The box is 100 wide and the ball 10, so it may travel to x = 90. In step 3 it
reaches 97.5, is mirrored to 82.5 and moves left from then on.

## Rectangles that overlap

Most arcade collisions use *axis-aligned rectangles* (AABB: their edges are
parallel to the axes). Two rectangles overlap exactly when their x ranges
overlap *and* their y ranges overlap, and two ranges `[a, a + w)` and
`[b, b + w2)` overlap when each one starts before the other ends:

```dc example=overlap
```

The test uses `<`, not `<=`, so rectangles that only touch along an edge do
not count as overlapping. Choose that rule once and keep it.

## Collision response

Knowing that two things overlap is only half of it. The **response** decides
what happens next. Two common ones:

- *Push out and bounce*: move the moving object back out along the axis where
  it entered the other one, and reverse its velocity on that axis. To find the
  axis, measure how deep the overlap is in x and in y; the smaller of the two
  is the side the object came in from.
- *Steer with the hit position* (Pong): when the ball hits a paddle, the
  vertical speed comes from *where* on the paddle it hit: the farther from the
  paddle's center, the steeper the angle. Reversing the horizontal velocity
  and setting the vertical one from the offset gives the paddle control.

## Randomness you can test

A game that rolls dice with random numbers cannot be checked: every run
differs. A *pseudo-random* generator is a function from the previous number
to the next one, so the same *seed* always gives the same sequence. The
smallest useful one is a linear congruential generator: multiply, add,
let the `u32` wrap around. The high bits are better than the low bits, so
use a shift before taking the remainder:

```dc example=seeded
```

Seed 42 twice gives the same rolls; another seed gives others. Games keep the
`Rng` in their state, seed it from the clock at startup and from a constant in
tests. `std::os::random` gives real random bytes from the operating system
when you do not need a repeatable run.

## When things go too fast

Collision tests compare positions at the *ends* of steps. A fast, small
object can be on one side of a thin wall in one frame and on the other side
in the next and never overlap it: it *tunnels*. Keep steps small (the fixed
steps of the last chapter help), keep objects thick enough or run the update
several times per frame.

## Common mistakes

- **Integration before collision, in the wrong order.** Move first, then test
  and correct the new position.
- **Flipping the velocity without moving the object out.** It collides again
  next frame and flips back and forth inside the wall.
- **`<=` in the overlap test.** Resting contact then counts as a collision.
- **Normalizing a zero vector.** Check for length 0 first.
- **Mixing `i32` and `f32`.** Convert explicitly with `as f32` where integer
  data meets float positions.
- **Using the low bits of a generator.** `next % 6` repeats in a short cycle;
  shift first.
