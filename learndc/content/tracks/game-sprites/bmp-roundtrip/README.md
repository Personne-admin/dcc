# Save and load a picture

Pictures usually start life in a file. This exercise follows the whole route with a picture the program makes itself:
build a **surface** in CPU memory, write it to a BMP file, load the file back into a surface and upload that surface as a
**texture**. It teaches the one thing the surface and texture handles ask of you: destroy what you create, on every path.

Edit `src/bmpio.dc`. `last_error()` is provided: it turns `SDL_GetError()` (a C string) into a DC slice you can print.

## make_gradient

`make_gradient(size)` returns a new `size` by `size` surface in the format `SDL_PIXELFORMAT_ABGR8888` (bytes in memory:
R, G, B, A) or `null` if SDL cannot create it. Pixel (x, y) gets red `x * 60`, green `y * 60`, blue 100 and alpha 255.

A surface has `pixels` (a `void*`), `pitch` (bytes per row, which SDL may pad) and `w`, `h`. Write through
`u8* bytes = surface.pixels as u8*;` and compute each pixel's start as `y * surface.pitch + x * 4`: use the surface's own
pitch, not `size * 4`.

## save_gradient

`save_gradient(path)` makes a 4 by 4 gradient and writes it with `SDL_SaveBMP(surface, path)`. It returns whether the
save worked. The surface is a temporary: it must be destroyed with `SDL_DestroySurface` whether the save succeeded or
not, so put a `defer` right after the null check.

## load_texture

`load_texture(renderer, path)` loads the BMP with `SDL_LoadBMP(path)`, uploads it with
`SDL_CreateTextureFromSurface(renderer, surface)` and returns the texture (nearest-neighbour scaling, so the pixels stay
sharp). A missing file or a failed upload returns `null`. The loaded surface is a temporary again: the texture holds its
own copy of the pixels, so the surface is destroyed with `defer` before the function returns, on the success path and on
every failing one. The caller owns the returned texture.

## What you should see

A black window with a 160 by 160 picture: red grows to the right, green grows downward and the blue is constant. The
window shows the file that `save_gradient` wrote (`gradient.bmp`, in the lesson workspace) and `load_texture` read back.
`learndc run` opens the window; Esc quits.

The checks run offscreen: one reads the surface pixels directly, one saves, loads and renders through SDL's software
renderer and reads pixels back, and one loads a file that does not exist. None of them tests an on-screen window.
