# Why this works

A surface and a texture are separate copies of the same picture, so each has its own lifetime. The surface is CPU memory
you can write and save; the texture is the renderer's copy. Once `SDL_CreateTextureFromSurface` has returned, the texture
no longer depends on the surface, which is why `load_texture` can destroy the surface before it returns the texture.

`defer` placed right after the null check is the whole cleanup strategy. `load_texture` has three exits (the file is
missing, the upload fails, success) and the surface is destroyed on the last two without any exit repeating the call. Add a
fourth exit later and it is covered. `make_gradient` is the opposite case: it returns the surface, so ownership moves to
the caller, and the callers (`save_gradient` and the check) destroy it.

Writing pixels with the surface's own `pitch` matters because SDL may pad rows. For a 4 pixel wide ABGR surface the pitch
happens to be 16, but code that assumes `width * 4` breaks the day it is not; reading the pitch costs nothing.

BMP is a simple, lossless format that SDL3 reads and writes without extra libraries, which makes it a good first file
format. A real game loads its sprites once at startup, turns each into a texture, and destroys the surfaces at once.
