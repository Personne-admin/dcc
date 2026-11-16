## Hint 1

An event is a union: first look at `ev.type`, then read the view that goes with it. Write one `if` / `else if` chain, one branch per kind of event.

## Hint 2

For the keys: `bool down = ev.key.down;` and compare `ev.key.scancode` with the scancodes. Held keys do `in.left = down;`. Escape and Space only act when `down` is true, and Space also requires `!ev.key.repeat`. `pump` is `begin_frame(in);` plus a `while` loop around `SDL_PollEvent`.

## Hint 3

```
} else if ev.type == (sdl3::SDL_EventType::SDL_EVENT_MOUSE_BUTTON_DOWN as u32) {
    if ev.button.button == sdl3::SDL_BUTTON_LEFT {
        in.clicked = true;
        in.mouse_x = ev.button.x;
        in.mouse_y = ev.button.y;
    }
}
```
