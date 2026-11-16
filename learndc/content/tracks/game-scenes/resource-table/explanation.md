# Why this works

The table is the single owner of its textures: `main` owns the table, the table's functions own the textures in it. That
gives two small contracts. `unload` can always be called, because it only touches what exists and clears each field after
releasing it: a second call finds only nulls. `load_with` either returns `true` with everything created, or `false` with
nothing alive, because each failing branch calls `unload` before returning. The caller's one `defer resources::unload(&res)`
is then correct on every path, including when `load` failed halfway.

Clearing the field after destroying the texture is not tidiness: a destroyed texture is a dangling pointer, and a
second `SDL_DestroyTexture` on it is undefined behaviour. A `null` field is a pointer you can still test.

Destroying in the reverse order of creation (key, hero, wall) is a habit that pays off when one resource depends on
another. The textures here are independent of each other, but they all depend on the renderer, which is why `main` writes
the table's `defer` after the renderer's.

The checks cannot ask SDL how many textures are still alive, so they cannot catch a `load_with` that nulls a pointer
without destroying the texture. That part is the discipline of this lesson: every `null` you write has a `SDL_DestroyTexture`
before it.
