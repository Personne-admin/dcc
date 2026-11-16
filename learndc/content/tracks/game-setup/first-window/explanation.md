# Why it works

`open` follows the rule "check every call, undo what succeeded". The steps form a stack: Init, window, renderer. A failure at step N must undo steps N-1 down to 1, in that order, which is exactly what the three failing branches do. The caller never sees a half-open state: `open` either returns `true` with two valid handles or `false` with SDL shut down and `g` empty. That is why a second `open` after a failure works, which the check proves.

`defer` is the right tool when a function owns a resource for its own lifetime, like `main` owning the `Gfx`. Here `open` hands ownership to its caller on success, so cleanup on the failure paths is written by hand. In `main`, `defer gfx::close(&g)` right after the successful `open` takes over: it runs on every way out of `main`.

`close` mirrors `open` backwards: renderer, then window, then `SDL_Quit`. Destroying a window before its renderer would leave the renderer pointing at a dead window. Clearing the pointers and testing them for `null` makes a repeated `close` harmless instead of a double free.
