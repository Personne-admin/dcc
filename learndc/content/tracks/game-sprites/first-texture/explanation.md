# Why this works

A texture is the renderer's own copy of a picture. `SDL_CreateTexture` reserves it, `SDL_UpdateTexture` copies your bytes in, and
from then on drawing it costs one call per frame instead of copying pixels. The pitch tells SDL how many bytes one row of *your*
array takes; with four bytes per pixel and no padding that is `SIZE * 4`.

`make_texture` hands the texture to its caller, so it must not destroy it on success. On the failing path it is still the owner,
which is why it destroys the half-built texture before returning `null`: otherwise the handle is lost. Success transfers ownership,
failure cleans up. `main` then writes `defer sdl3::SDL_DestroyTexture(texture)` right after the null check, so every way out of
`main` releases it before the renderer goes away.

Nearest-neighbour scaling copies the closest source pixel, so each of the 16 pixels becomes a crisp 40 by 40 square. With the
default linear filter the edges between squares would be blends of orange and blue, and the checks at x = 119 and x = 120 would
see them.
