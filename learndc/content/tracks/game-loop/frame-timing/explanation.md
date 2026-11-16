# Why it works

SDL's counters count up forever, so a single reading means nothing; only the difference between two readings does. `tick` stores the previous reading in the `Clock`, which is state that must survive from one frame to the next. Subtracting two `u64` values before converting keeps the precision: nanosecond counters get far too big for an `f32`, and a difference of a few million does not.

The clamp is a safety rail. The physics of a frame assumes `dt` is small; a debugger pause, a dragged window or a suspended laptop produce a `dt` of seconds, and `x += speed * dt` would move objects across the whole level, through every wall. Losing a quarter of a second of game time is harmless; teleporting is not.

The accumulator makes the *simulation* independent of the frame rate: time arrives in uneven chunks, updates leave in equal steps. Keeping the remainder means no time is lost or invented. The cap handles the worst case, a machine that needs more than one step of real time to compute one step of simulated time; without it each frame would owe more steps than the last. Dropping the backlog makes the game slow down instead of freezing.
