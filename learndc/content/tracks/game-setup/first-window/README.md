# Open a window and close it again

Every SDL program starts the same way: initialize, create a window, create a
renderer, and at the end undo all of it. This lesson turns that into two
functions, `open` and `close`, in `src/gfx.dc`. `src/main.dc` uses them and is
a complete little program: a window that is cleared to dark blue until you
press Esc or close it.

## What the three handles are

- `SDL_Init(SDL_INIT_VIDEO)` starts the video subsystem. It returns `bool`.
- A **window** (`SDL_Window*`) is the rectangle the operating system shows.
  `SDL_CreateWindow(title, width, height, flags)` returns `null` on failure.
- A **renderer** (`SDL_Renderer*`) draws into a window.
  `SDL_CreateRenderer(window, name)` returns `null` on failure; the name `null`
  means "choose the best one", and an unknown name fails.

The struct `Gfx` holds the two pointers so that `main` can hand them around.

## Task 1: open

`open(g, title, width, height, renderer_name)` returns `true` when both
handles exist and stored in `g`.

1. Start with `g.window = null` and `g.renderer = null`, so a failed `open`
   leaves nothing half-filled.
2. Call `SDL_SetMainReady()`, then `SDL_Init`. If it fails, return `false`.
3. Create the window. If that fails, undo the init with `SDL_Quit()` and
   return `false`.
4. Create the renderer. If that fails, destroy the window, call `SDL_Quit()`
   and return `false`.
5. Store both handles in `g` and return `true`.

Why not `defer`? A `defer` runs on every exit, including the successful one,
and on success the caller must receive a working window. Cleanup that depends
on *failure* is written out by hand, once per failing step, and it always
undoes the steps before it.

## Task 2: close

`close(g)` releases everything in the reverse order of creation: renderer,
window, then `SDL_Quit()`. Set the pointers back to `null` afterwards. Calling
`close` on an already closed `Gfx` must be harmless (check for `null` before
destroying).

## How the checks see it

The checks set the hint `SDL_VIDEO_DRIVER` to `offscreen`, SDL's driver that
runs all the calls without a screen (`SDL_Quit` resets every hint, so the
checks set it again before each new `SDL_Init`), and call `open` and `close`. They then
ask SDL: `SDL_WasInit` says whether the video subsystem is running. A second
check chooses a video driver that does not exist and a renderer name that does
not exist, to see that your error paths clean up.

Your own `learndc run` uses the real driver and opens a window on a desktop
session. Esc or closing the window quits.
