# The Moving Square

You are building a small interactive program: a square in a 640 x 480 window
that you steer, teleport and pause. Everything the chapter taught is in
here: the loop, the `Input` struct, delta time, mouse and keyboard, and
rendering. Each stage below is checked on its own; clear them in order and
watch them light up. A stage that passed earlier stays in your record, and
the objective list shows whether it still passes with your current code.

## What is already there

- `src/input.dc`: `Input`, `apply_event` and `pump`, the translation you wrote
  in *From SDL events to an Input struct*, with one more key: Shift sets
  `boost`.
- `src/timing.dc`: the `Clock` and `tick` from *Delta time and fixed steps*.
- `src/game.dc`: `State` (position of the top left corner, a color index and
  a paused flag), the constants (`WIDTH` 640, `HEIGHT` 480, `SIZE` 40, `SPEED`
  240 pixels per second, `COLORS` 4), the helper `axis`, and `update`, which
  calls your functions: `toggle_pause`, then (unless paused) `click`,
  `move_square` and `keep_inside`.
- `src/render.dc`: `Color`, and `palette(index)`, the four square colors.
- `src/main.dc`: opens the window and calls `runner::run`.

Keys and mouse of the finished game:

| Input | Effect |
|---|---|
| arrows or WASD | move the square |
| Shift (bonus) | move twice as fast |
| left mouse button | center the square on the pointer, next color |
| Space | pause or resume |
| Esc or closing the window | quit |

## Stage 1: Read the old cabinet's loop

Two questions about a legacy loop. No code to write.

## Stage 2: Move with the keys

`move_square(s, in, dt)` in `game.dc`. `dx = axis(in.left, in.right)` and
`dy = axis(in.up, in.down)` are -1.0, 0.0 or 1.0. The square moves by
`dx * speed * dt` and `dy * speed * dt`, where the speed is
`current_speed(in)` (240 pixels per second).

Moving diagonally adds the two movements, which makes the path 1.41 times
longer than a straight one. Fix that: when both `dx` and `dy` are not zero,
multiply both by `0.70710678`, so the total speed stays 240. Do not clamp
here; the next stage does.

## Stage 3: Stay inside the window

`keep_inside(s)` limits `s.x` to `0.0 ..= WIDTH - SIZE` and `s.y` to
`0.0 ..= HEIGHT - SIZE`. The position is the *top left* corner, so the right
limit leaves room for the square's own width.

## Stage 4: Click to teleport

`click(s, in)` does nothing unless `in.clicked`. On a click it centers the
square on `(in.mouse_x, in.mouse_y)` and moves to the next color:
`s.color = (s.color + 1) % COLORS`. (`update` clamps afterwards, so a click
near an edge still ends inside the window.)

## Stage 5: Pause

`toggle_pause(s, in)` flips `s.paused` whenever `in.pause_pressed` is true.
The given `update` skips everything else while paused.

## Stage 6: Draw the state

`draw(renderer, state)` in `render.dc`:

1. clear to (20, 24, 40),
2. fill the square at `(state.x, state.y)`, `SIZE` by `SIZE`, in
   `palette(state.color)`,
3. if `state.paused`, two white bars on top: x 10 and x 24, y 10, 7 wide and
   30 tall.

## Stage 7: Run the loop

`runner::run(renderer, state, max_frames)` in `runner.dc` is the loop itself.
Per frame: `input::pump`, leave if `in.quit`, `dt` from `timing::tick`
with `SDL_GetTicksNS()`, `game::update`, `render::draw`, `SDL_RenderPresent`,
`SDL_Delay(16)`. It returns the number of completed frames. A negative
`max_frames` means "until quit" (that is how `main` calls it); the check uses
small numbers. This stage uses the earlier ones: if movement or clicks do
not work, the loop check shows it.

## Bonus stage: Shift to sprint

`current_speed(in)` returns twice `SPEED` while `in.boost` is true.

## Polish (not checked)

Ideas to try in your own time: a trail of fading squares, a color that
follows the speed, a bounce at the walls instead of a stop, a second square
for WASD and a third for the arrow keys.

## Run it

`learndc run` opens the window (a desktop session is needed). Everything the
checks verify is logic and offscreen pixel reads plus a loop driven by events
the check pushes into SDL's queue; they do not press real keys or click a
real window. Playing it yourself is the complete test.
