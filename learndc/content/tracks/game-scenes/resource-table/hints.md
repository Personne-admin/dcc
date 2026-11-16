## Hint 1

Write `unload` first: it is the building block. Then `load_with` is three creations, each followed by a null check, and every failing branch is the same two lines.

## Hint 2

`unload`: for each field, `if r.field != null { destroy; r.field = null; }`. `load_with`: `r.wall = art::make(renderer, wall_art); if r.wall == null { unload(r); return false; }`, the same for the hero and the key, then `return true;`.

## Hint 3

```dc
r.hero = art::make(renderer, hero_art);
if r.hero == null {
    unload(r);
    return false;
}
```

`unload` destroys the key, the hero and the wall (reverse order of creation), each guarded by a null check, and sets each field to `null`.
