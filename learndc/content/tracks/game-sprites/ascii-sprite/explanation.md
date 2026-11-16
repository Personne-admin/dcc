# Why this works

Pixel art is data, and decoding it is the same job as any lookup: one function (`color_for`) answers "what does this
character mean", another (`decode`) walks the grid. Keeping the palette in its own function means you can change a color
in one place, add a new character without touching the loop, and test both pieces without SDL.

The offset `(y * width + x) * 4` is the pitch formula from the lecture: every row is `width` pixels, every pixel is four
bytes, so row `y` starts `y * width * 4` bytes in. Writing them in the order red, green, blue, alpha matches the
ABGR8888 format that `create` selected, where the memory order of the bytes is R, G, B, A.

Transparency is just alpha 0. A texture whose format has an alpha channel is blended onto the framebuffer when drawn, so
the dots in the art let the gray background show. An unknown character falls through to the same transparent value, which
makes a typo visible as a hole in the sprite instead of a crash.
