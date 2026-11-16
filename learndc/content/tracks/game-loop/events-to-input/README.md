# From SDL events to an Input struct

The rule of this path: game logic never sees an `SDL_Event`. `main` turns
what happened into a plain `Input` struct, and the game reads only that.
This lesson writes the translation. `src/input.dc` declares `Input` and
`begin_frame`, which clears the *one-shot* flags at the start of every frame.
`src/main.dc` is a finished demo: arrows or WASD move a small square, a click
teleports it, Space changes its color, Esc or closing the window quits.

```dc
public struct Input {
    bool left;  bool right;  bool up;  bool down;   // held
    bool quit;
    bool pause_pressed;                             // one frame
    bool clicked;                                   // one frame
    f32 mouse_x;  f32 mouse_y;                      // where the last click was
}
```

## Task 1: apply_event

`apply_event(in, ev)` looks at one event and updates `in`:

| Event | Effect |
|---|---|
| `SDL_EVENT_QUIT` | `quit = true` |
| key down or up, arrows or WASD | set `left`, `right`, `up` or `down` to `ev.key.down` |
| key down, Escape | `quit = true` (releasing Escape does nothing) |
| key down, Space, not a repeat | `pause_pressed = true` |
| left mouse button down | `clicked = true`, `mouse_x` and `mouse_y` from `ev.button` |
| anything else | ignored |

How to read an event:

```dc
if ev.type == (sdl3::SDL_EventType::SDL_EVENT_KEY_DOWN as u32)
        || ev.type == (sdl3::SDL_EventType::SDL_EVENT_KEY_UP as u32) {
    sdl3::SDL_Scancode code = ev.key.scancode;
    bool down = ev.key.down;
    ...
}
```

`ev.type` is a `u32`, the event constants are an enum, hence `as u32`. For
keys use `ev.key` (`scancode`, `down`, `repeat`); for the mouse use
`ev.button` (`button`, `x`, `y`). The scancodes are
`sdl3::SDL_Scancode::SDL_SCANCODE_LEFT`, `..._A`, `..._SPACE`,
`..._ESCAPE`, and so on; the left mouse button is `sdl3::SDL_BUTTON_LEFT`.

A *held* key is state: write `down` into it on both key down and key up. A
*pressed* flag is an action: it is only set on the key down, and `begin_frame`
clears it every frame, so holding Space produces a single press.

## Task 2: pump

`pump(in)` is the per-frame call: `begin_frame(in)`, then drain the queue:

```dc
sdl3::SDL_Event ev = {};
while sdl3::SDL_PollEvent(&ev) {
    ...
}
```

and hand every event to `apply_event`.

## How the checks work

The first check builds `SDL_Event` values by hand and calls `apply_event`;
no SDL is initialized. The second initializes SDL with the offscreen video
driver, puts real events into SDL's own queue with `SDL_PushEvent`, calls
your `pump` and looks at the `Input`: the queue, `SDL_PollEvent` and your
translation all run for real. It does not press real keys; with `learndc
run` you do.
