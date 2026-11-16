# Why it works

The generator is a plain function from the old state to the new one, which is why it is repeatable: the whole "randomness" of a run is the seed. The multiplier and the increment are chosen so that the sequence visits every 32-bit number before repeating, and the wrap-around of `u32` arithmetic is the modulo operation the formula calls for, for free.

The low bits of such a generator are weak: bit 0 simply alternates between 0 and 1, bit 1 repeats every four numbers, and so on. A range computed with `% span` on the low bits would show patterns, for example a die that alternates odd and even. Shifting right by 16 first uses the better high bits.

`unit` takes 24 bits because an `f32` has a 24-bit significand: every whole number below 2^24 converts exactly, and dividing by 2^24 is exact too, so the result is always below 1. `chance` reuses `range` to stay in whole numbers and avoid float comparisons.

In a game, the `Rng` lives in the game state, next to everything else that changes. A check (or a replay) then creates the same state with the same seed and expects the same game, which is how the Pong boss fight checks its serves.
