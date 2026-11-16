# Reading a hash set

## What to look for

A hash set answers "is this key in the set?" quickly. The key is turned into a slot number (here `key % 8`); in the best case the key is in that slot and one comparison is enough. Different keys can land on the same slot: a collision. This set resolves collisions by linear probing: try the next slot, then the next, wrapping around.

Real library maps are built from the same ideas with growing storage and better hash functions. This version has a fixed capacity of eight so that you can trace every slot by hand.

## The program

```dc example=hashset
```

## Reading it

- `home_slot` maps a key to the slot where it prefers to live.
- `find` starts at the home slot and probes forward. An unused slot means the key cannot be further on, so the search ends with `None`; a slot holding the key ends it with `Some(slot)`.
- `insert` first asks `find`, so duplicates are refused. It then walks to the first unused slot from the home slot and claims it.
- `add` and `lookup` only print.

Trace the insertions: 3 lives in slot 3. 11 also has home slot 3, which is taken, so it moves on to slot 4. 19 probes 3, 4 and settles in slot 5. The duplicate 11 is refused by `find`. 4 has home slot 4, which 11 occupies, so it probes on to 5 (taken) and settles in slot 6.

The same probing order is used by `insert` and `find`. That is the invariant: a key is always reachable from its home slot without crossing an unused slot.

## Common mistakes

- Expecting a key to be in its home slot. After collisions it can be several slots further on.
- Stopping `find` at the first slot that holds a different key. Only an unused slot ends the search early.
- Forgetting the wrap-around `(slot + 1) % 8`; without it the probe would leave the array.
- Looping forever in `insert` when the table is full. The `count == 8` check before the loop prevents it.
