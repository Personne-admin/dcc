# A seeded random number generator

A game wants surprises: where a star sits, which way a ball is served, how
often an enemy shoots. A *pseudo-random generator* gives numbers that look
random but are a fixed function of the generator's state. Start from the same
**seed** and you get the same numbers: that makes a bug reproducible and a
check possible. The operating system also offers real randomness
(`std::os::random`), but this is the right tool when a run must be repeatable.

`src/rng.dc` has the `Rng` struct (just a `u32` state) and placeholders.
`src/main.dc` is a finished toy: it scatters 60 stars from one seed; Space
picks a new seed from the clock; the same seed always draws the same sky.

## The generator

A *linear congruential generator* multiplies the state, adds a constant and
lets the number wrap around at 32 bits:

```text
state = state * 1664525 + 1013904223
```

`u32` arithmetic in DC wraps around (it does not trap), which is exactly the
modulo 2^32 the formula needs.

## Task 1: seeded and next

- `seeded(seed)` returns an `Rng` whose state is `seed`.
- `next(r)` applies the formula to `r.state`, stores the new state and returns
  it. Seed 42 gives 1083814273, 378494188, 2479403867, 955863294, 1613448261.

## Task 2: range

`range(r, lo, hi)` returns an integer from `lo` to `hi` **inclusive**. There
are `span = hi - lo + 1` possible values, so compute

```text
lo + ((next(r) >> 16) % span)
```

The `>> 16` matters. In this kind of generator the low bits cycle quickly (the
lowest bit just alternates), so `next % 6` would look patterned. The high bits
are much better. `span` is an `i32` in the signature but the arithmetic needs
`u32`, so convert with `as u32` and `as i32`. Seed 42 with `range(1, 6)` rolls
2, 4, 3, 6, 2.

## Task 3: unit and chance

- `unit(r)` returns an `f32` from 0 (included) up to 1 (excluded): take the
  high 24 bits with `next(r) >> 8`, convert with `as f32` and divide by
  `16777216.0`, which is 2 to the power of 24. An `f32` holds exactly 24 bits
  of whole numbers, so the result is exact and can never reach 1.
- `chance(r, percent)` is true `percent` times in 100: `range(r, 0, 99) < percent`.

## Where the seed comes from

A game seeds from something that changes between runs (the clock:
`SDL_GetTicks()` in `main.dc`) when it wants variety, and from a constant when
it wants repeatability: tests, replays, a "daily challenge". The generator
does not care; it only needs *some* `u32`.
