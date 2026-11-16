## Hint 1

`read_keyboard` is four assignments. `axis` is two ifs. `button_down` is one expression with a shift and a bit-and.

## Hint 2

`held.left = keys[...LEFT as usize] || keys[...A as usize];` and so on for the other directions. For the mask: shift 1 left by `button - 1` places, and the button is down when `mask & thatbit` is not zero. `button` is a `u8`; convert it with `as u32` before shifting.

## Hint 3

```
return (mask & (1 << ((button as u32) - 1))) != 0;
```
