# Why this works

The animation is a pure function of time: the same elapsed milliseconds always give the same frame. That is why
`frame_at` can be tested with numbers and why the animation runs at the same speed at 30 or 144 frames per second. The
loop only measures how much time passed (`dt`) and feeds it to `advance`.

Integer division does the stepping: `elapsed / frame_ms` is the count of whole frames that have passed, and the
remainder is time inside the current frame. A loop wraps with `%`, a one-shot animation clamps, and both are one line.

Bounding `elapsed` is the detail that keeps long sessions honest. A looping animation that kept adding would work for a
long time and then overflow its `u32` (about 49 days of milliseconds), and a one-shot animation that kept adding would do
the same. Wrapping or clamping at every `advance` makes the number's size independent of how long the game runs.

`finished` is what a scene uses to react to the end of an animation, for example to leave a death animation and show the
game over screen. It is a query on the state, not a callback, so the scene decides when to look.
