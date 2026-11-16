# Why it works

An `SDL_Event` is a union: the same memory is a key event, a mouse event or a quit event. `ev.type` is the one field that is valid for all of them, so every translation starts there and only then reads the matching view. Reading `ev.key` for a mouse event would give you meaningless bytes.

Two kinds of data come out. Held keys are state: both key down and key up write the same boolean, so `left` is true exactly while the key is down. One-shot actions are *edges*: `pause_pressed` is set when the key goes down and `begin_frame` clears it at the start of the next frame. The operating system repeats key-down events while a key is held; `ev.key.repeat` marks those, and ignoring them is what makes one press toggle the pause once.

`pump` is the boundary between SDL and the game. Everything SDL-specific, the queue and the union, stays inside `input.dc`. The game logic that comes next only sees an `Input` and can be tested by building one by hand. Clearing the edges *before* draining (not after) means they last for exactly the frame in which the events were handled, which is when `update` reads them.
