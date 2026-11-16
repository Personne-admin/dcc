# Why big numbers work this way

A number in base 10^9 is just a polynomial whose "digits" are `u32` limbs, and
the school algorithms work unchanged: add column by column with a carry, multiply
each pair of digits into the right column, subtract with a borrow, divide from the
top with a remainder. The base 10^9 is chosen for two reasons. A limb times a limb
is below 10^18, which fits in a `u64` together with a carry, so the products never
overflow the intermediate type. And a limb is exactly nine decimal digits, so
printing needs no base conversion.

The normal form is the contract that keeps the functions composable. If every
operation returns numbers without leading zero limbs, with zero as one limb, then
`compare` can look at lengths first, `to_text` can print the top limb unpadded and
the tests can compare limb arrays directly. The cost is a `trim` after any operation
that can shrink or leave leading zeros (`sub`, `mul`, `divmod_small`).

Aliasing is the hazard of a memory-owning API. `add(&x, &x)` must give 2x even
though `a` and `b` are the same array and `a` may grow, and so move its storage,
in the middle of the loop. The safe habit is to read through `items()` each time you
need a limb, never to hold a slice across a `push`.

The two typed errors say exactly what can go wrong in arithmetic on naturals:
there is no negative result and no division by zero. Both are refusals that leave
the number unchanged, so a caller can try something else. Allocation failure comes
through `@implicit_construction`, with no extra code in the functions that can only
fail that way.
