## Hint 1

Everything is school arithmetic with a bigger digit. Decide the invariants first
(limbs below the base, no leading zero limb, zero is one limb) and write one small
private helper that restores them, so no function has to remember the rule.
Do every intermediate computation in `u64` and cast back to `u32` only when the
value is known to be below the base.

## Hint 2

`add` is a loop over the longer length that adds `carry`, the limb of `a` if it has
one and the limb of `b` if it has one, stores `sum % BASE` and keeps `sum / BASE` as
the carry; a final carry becomes a new top limb. For `mul`, give `out` `a.len + b.len`
zero limbs, add each product into the right position and propagate the carry upward.
`sub` and `divmod_small` mirror the same idea in the other direction.

## Hint 3

Aliasing: `add(&x, &x)` passes the same array as `a` and `b`. Fetch limbs through
`items()` inside the loop, after any push has happened, and do not keep a slice from
before a push. `to_text` prints the top limb without padding and every other limb
padded to nine digits, which you can do by extracting digits with a scale of
100000000 down to 1.
