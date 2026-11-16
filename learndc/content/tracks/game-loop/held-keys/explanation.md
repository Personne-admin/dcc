# Why it works

The keyboard state is a snapshot: one `bool` per scancode, updated whenever SDL processes events (`SDL_PollEvent` does that). Indexing by scancode is cheap and gives "is it held" without keeping your own copy. That is why movement uses the state while one-shot actions use events: the state cannot tell you that a key *went* down between two frames, only that it is down now.

`axis` is the usual trick for movement: two booleans become one signed number that can be multiplied by a speed. Making "both keys" give 0 means that pressing left and right together stops the object instead of letting one key win.

The mouse buttons are packed into the bits of one integer, one bit per button, so one call returns all of them. Button `b` lives in bit `b - 1` because SDL numbers buttons from 1. `1 << (b - 1)` builds a mask with only that bit set; `&` keeps just that bit of the state, and the button is down when anything is left.
