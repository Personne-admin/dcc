# Your first texture

`checker.fill_pixels` already writes the 64 bytes of a 4 by 4 checkerboard (orange and dark blue, four bytes
per pixel: red, green, blue, alpha). Your job is to turn those bytes into a texture and draw it.

Edit `src/checker.dc`.

## make_texture

`make_texture(renderer)` creates a texture the renderer can draw and returns it, or `null` if anything fails.

1. `SDL_CreateTexture(renderer, format, access, w, h)` with `SDL_PIXELFORMAT_ABGR8888` (bytes in memory: R, G, B, A),
   `SDL_TEXTUREACCESS_STATIC` and `SIZE` by `SIZE` pixels. A `null` result means SDL could not create it.
2. Fill a `u8[64]` array with `fill_pixels(&pixels[0])` and upload it with
   `SDL_UpdateTexture(texture, null, &pixels[0] as const void*, pitch)`. The pitch is the bytes per row: `SIZE * 4`.
   If the upload fails, destroy the texture before returning `null`, otherwise it leaks.
3. `SDL_SetTextureScaleMode(texture, SDL_ScaleMode::SDL_SCALEMODE_NEAREST)` keeps enlarged pixels sharp.

The caller (`main`) owns the returned texture and destroys it with `defer`.

## draw

`draw(renderer, texture)` already clears the window to black. Add the checkerboard, enlarged into a 160 by 160
rectangle whose top left corner is at (80, 40):

```dc
sdl3::SDL_FRect dst = {x = 80.0, y = 40.0, w = 160.0, h = 160.0};
sdl3::SDL_RenderTexture(renderer, texture, null, &dst);
```

A `null` source rectangle means the whole texture.

## Expected picture

A 320 by 240 black window with a 160 by 160 checkerboard in the middle: sixteen 40 by 40 squares, orange where
the row and column sum is even and dark blue where it is odd, with sharp edges. Run `learndc run` (or `r` in the
lesson screen) to see it in a window; Esc or closing the window quits.

The checks create an offscreen window, call your code once and read pixels back from SDL's software renderer.
That proves your SDL calls draw the right pixels; it is not a test of an on-screen window.
