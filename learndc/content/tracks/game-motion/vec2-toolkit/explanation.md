# Why it works

A `Vec2` is nothing special to the compiler: two `f32` fields and some functions. Wrapping the pair makes the code read like the maths: `add(position, scale(velocity, dt))` is one thought instead of two lines for x and two for y, and a mistake like moving x but forgetting y becomes impossible.

`dot` and `length` are related: the length is the square root of a vector dotted with itself. `normalized` divides by the length, which keeps the direction and fixes the length at 1, so `scale(normalized(d), speed)` moves at exactly `speed` however far away the target is. Checking for a zero length first matters: `0 / 0` is `NaN` in floating point, and a `NaN` position is never equal to, less than or greater than anything, so every later comparison would be false and the object would vanish.

`integrate` changes the velocity before the position. The position then uses the velocity the object has at the *end* of the step, which makes simple falling and bouncing stable. The order is checked by numbers: with gravity 200 and `dt` 0.1 the velocity is 20 after one step and the position has already moved by 2, not 0.
