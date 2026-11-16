# Resources and lifetimes

A game owns things that SDL (or the operating system) lends it: a window, a renderer, textures, files. Each one
must be given back exactly once. This lecture is about *who* does that and *when*, with a made-up resource so that
the counting is visible. The exercise afterwards does it with real textures.

## Count what is alive

The examples use a `Pool`: `acquire` hands out a numbered handle and counts it as alive, `release` counts it as
returned, and `0` means "no handle". The program prints how many are still alive. The real counterparts are
`SDL_CreateTexture` and `SDL_DestroyTexture`.

## Every exit is an exit

```dc example=early_return_leaks
```

`use_both_leaky` releases its two handles at the end, which is fine as long as nothing returns earlier. The second
acquire can fail, and that early `return false` skips both releases of what was already acquired: one handle is lost
for the rest of the program. The `defer` version writes each release right after the acquire it belongs to, so every
exit, the early one included, releases what exists at that moment. Note that the second `defer` comes after the
check: a handle that was never acquired is never released.

The rule is the one from the memory chapter, now applied to SDL: acquire, check, defer the release.

## Ownership moves with the return value

`defer` fits when the function is the owner. A function that *creates* something for its caller (`make_texture`,
`load_texture`) hands the ownership over by returning it, and then must not release it on the success path. It
still owns the half-built thing on its failing paths, so it cleans those up itself. The caller takes over with its
own `defer`.

## A table of resources

A game usually has several resources that live together: the tile textures, the hero, the key. A small struct holds
them, with two functions for the whole lifetime.

```dc example=resource_table
```

- `load` is **all or nothing**. When the third acquire fails, the first two are already alive; `load` calls
  `unload` before it returns `false`, so the caller never has to wonder what is half loaded.
- `unload` releases only the handles that exist and sets each field back to `0` afterwards. A second `unload` finds
  nothing to release, so calling it again, or after a failed `load`, is harmless.
- Resetting the fields matters twice: it makes the cleanup repeatable, and it stops stale handles from being used
  after release.

Zeroing the table before the first load (`Table table = {};`) is what makes "release only what exists" work.

## Who owns what

For a larger game, decide ownership once and write it down:

| Resource | Created by | Destroyed by | Lives as long as |
|---|---|---|---|
| Window, renderer | `main` | `main`, with defer | the program |
| Textures table | `load` in `main` | `main`, with `unload` | the program (or one level) |
| Per-level data (map) | the level loader | the level owner | one level |

Resources shared by every scene belong to the outermost owner; scenes borrow them through pointers and never destroy
them. Destruction order is the reverse of creation: textures before the renderer, the renderer before the window.

## Common mistakes

- Putting the release at the end of the function instead of next to the acquire.
- Releasing a handle on the success path although the function returns it to the caller.
- A `load` that fails halfway and leaves the first resources alive.
- An `unload` that does not clear its fields, so the second call releases the same handle twice.
- Destroying a texture after its renderer.
