# Held keys and mouse buttons

Events tell you when something *changes*. To move a character while a key is
held, it is simpler to ask what is down *right now*. SDL keeps that for you.
`src/controls.dc` has three placeholders; `src/main.dc` is a finished demo
that uses them: arrows or WASD move a square, holding the left mouse button
makes it yellow, and a small outline follows the mouse.

## The keyboard state

`SDL_GetKeyboardState(&count)` returns a pointer to an array of `bool`, one
per scancode. SDL owns it and updates it as events are pumped; you only read
it. The value at index `scancode` is `true` while that key is down:

```dc
i32 count = 0;
const bool* keys = sdl3::SDL_GetKeyboardState(&count);
bool left = keys[sdl3::SDL_Scancode::SDL_SCANCODE_LEFT as usize];
```

## Task 1: read_keyboard

`read_keyboard(keys, held)` fills the `Held` struct: `left` is true when the
left arrow *or* A is down, `right` for the right arrow or D, `up` for the up
arrow or W, `down` for the down arrow or S. It writes all four fields every
call, so nothing stays stuck.

The checks pass an array of 512 `bool` values, as large as SDL's state.

## Task 2: axis

Two opposite keys make one number: -1 when only the negative key is down, 1
when only the positive one is, and 0 when neither or both are:

```dc
x += (controls::axis(held.left, held.right) * 4) as f32;
```

## Task 3: button_down

`SDL_GetMouseState(&x, &y)` stores the pointer position and returns a bit
mask of the buttons that are down. Button number `b` (`SDL_BUTTON_LEFT` is 1,
middle is 2, right is 3) is bit `b - 1`, so the value of the bit is
`1 << (b - 1)`. `button_down(mask, button)` returns whether that bit is set:
combine with `&` and compare to zero.

## Try it

`learndc run`, move the square, hold the left mouse button over the window.
Notice what happens at diagonals: the square is faster because both axes
move at full speed. The boss fight of this chapter fixes that.
