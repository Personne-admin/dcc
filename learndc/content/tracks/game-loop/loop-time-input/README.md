# The game loop, input and time

A program you have written so far runs once from top to bottom. A game is
different: it runs *until the player quits*, and in every pass it must look
at the player, move the world and draw a new picture. This lecture covers
that loop, the input SDL delivers and the clock that keeps motion steady.

## The loop

Every frame of every game has the same four jobs:

1. **Events**: ask SDL what happened (a key, a click, the window closing).
2. **Update**: change the game state based on the input and the elapsed time.
3. **Draw**: render the state into the back buffer.
4. **Present**: show it (`SDL_RenderPresent`).

The loop below is a simulation without a window, so you can see it run. A
scripted *queue* stands in for SDL's event queue: two "right" events arrive
in frame 1 and a "quit" in frame 3. Look at the shape: an outer `while
running`, and an inner `while poll(...)` that drains *all* pending events
before the frame goes on.

```dc example=loop_skeleton
```

Draining everything matters: if you took one event per frame, a burst of
events would pile up and the game would answer seconds late. Leaving the
loop is just setting `running = false`; `defer` then cleans up as usual.

## Events in SDL3

In SDL the pieces look like this:

```dc
sdl3::SDL_Event ev;
while sdl3::SDL_PollEvent(&ev) {
    if ev.type == (sdl3::SDL_EventType::SDL_EVENT_QUIT as u32) {
        running = false;
    } else if ev.type == (sdl3::SDL_EventType::SDL_EVENT_KEY_DOWN as u32) {
        if ev.key.scancode == sdl3::SDL_Scancode::SDL_SCANCODE_ESCAPE {
            running = false;
        }
    }
}
```

`SDL_Event` is a *union*: one block of memory that can be read as many
different structs. `ev.type` is always valid and tells you which view to
use: for a key event read `ev.key`, for a mouse button `ev.button`, for mouse
motion `ev.motion`. `SDL_PollEvent` returns `true` while it filled `ev` and
`false` when the queue is empty.

The events you meet first:

| Type | View | Useful fields |
|---|---|---|
| `SDL_EVENT_QUIT` | `ev.quit` | none: the last window was closed |
| `SDL_EVENT_KEY_DOWN`, `SDL_EVENT_KEY_UP` | `ev.key` | `scancode`, `key`, `down`, `repeat` |
| `SDL_EVENT_MOUSE_BUTTON_DOWN`, `_UP` | `ev.button` | `button`, `x`, `y`, `clicks` |
| `SDL_EVENT_MOUSE_MOTION` | `ev.motion` | `x`, `y`, `xrel`, `yrel` |

A key has two names. The **scancode** is the physical key (the key where W
sits on a US keyboard, wherever the letter is printed on yours); the
**keycode** is what the key means in the current layout. Use scancodes for
movement (WASD stays under the left hand on every layout) and keycodes for
things like Esc.

## Events or keyboard state?

There are two ways to ask about keys. *Events* tell you when something
*changed*; the *keyboard state* (`SDL_GetKeyboardState`) tells you what is
down *right now*, as an array indexed by scancode:

```dc
i32 count = 0;
const bool* keys = sdl3::SDL_GetKeyboardState(&count);
if keys[sdl3::SDL_Scancode::SDL_SCANCODE_LEFT as usize] {
    // the left arrow is held
}
```

Holding a key to move wants the state: it is true every frame the key is
down. A single action such as "pause" or "jump" wants an event, because
holding Space must not toggle the pause 60 times a second. A key event also
carries `repeat`, true for the repeats the operating system produces while a
key is held; ignore those for one-shot actions. The example keeps both kinds
in one `Input`: *held* values stay until the key goes up, *pressed* values
are cleared at the start of every frame.

```dc example=held_and_pressed
```

## The mouse

A mouse button event carries the button (`SDL_BUTTON_LEFT` is 1) and the
position in *window* coordinates, as floats. For the position now, ask
`SDL_GetMouseState(&x, &y)`; it returns a bit mask of the buttons that are
down, where button `b` is bit `b - 1`. With logical presentation, convert
the position as you did in the drawing chapter.

## One plain Input struct

SDL types are awkward to test and invite SDL calls everywhere. The pattern
of this path: `main` turns events and key state into a plain `Input` struct
of booleans and numbers, and the game state changes in a function that takes
only data: `update(State*, Input, f32 dt)`. The game logic then has no SDL in
it, so a test can call it with a made-up `Input` and look at the state. That
is exactly how the checks in this path test games.

## Time

Computers differ: your loop may run at 30, 60 or 144 frames per second. If
a square moves 5 pixels *per frame*, it is five times faster on a 144 Hz
machine than at 30. Scale motion by the time the last frame took instead:
`x += speed * dt`, with `speed` in pixels per second and `dt` in seconds.

```dc example=delta_time
```

SDL gives you the clock: `SDL_GetTicks()` is milliseconds since SDL started,
`SDL_GetTicksNS()` nanoseconds (both `u64`), and
`SDL_GetPerformanceCounter()` with `SDL_GetPerformanceFrequency()` is a
high-resolution counter in ticks per second. Compute `dt` as the difference
between two readings. Clamp it, say to a quarter of a second: after a pause
or a debugger stop, one giant `dt` would throw everything across the world.

To not spin the processor, wait between frames: either `SDL_Delay(16)`, or
ask the renderer for *vsync* with `SDL_SetRenderVSync(renderer, 1)`, which
makes `SDL_RenderPresent` wait for the display's refresh.

## Fixed steps

Variable `dt` is simple, but physics can behave differently at different
frame rates (a fast ball can skip through a wall in one long step). Many
games use a *fixed step* for the simulation: add each frame's time to an
*accumulator* and run `update` once per full step that fits, then keep the
remainder.

```dc example=fixed_step
```

Frame 2 took 33 ms and ran three 10 ms updates; the leftover time carried
over to the next frame. The picture is drawn once per frame, the simulation
advances in equal steps, so the same inputs always give the same game.

## Common mistakes

- **Reading one event per frame.** Drain the queue with `while SDL_PollEvent`.
- **Motion per frame instead of per second.** Multiply by `dt`.
- **Using key events for held movement or the state for one-shot actions.**
  Held movement wants the state, actions want events with `repeat` ignored.
- **Not clearing "pressed" flags.** A click that is never reset moves the
  square every frame.
- **An unclamped `dt`.** One long pause becomes one huge jump.
- **Quitting only on Esc.** Also quit on `SDL_EVENT_QUIT`, or the window's
  close button does nothing.
- **Comparing `ev.type` to an enum without `as u32`.** `ev.type` is a plain
  `u32`; the constants are an enum.
