## Hint 1

A texture is created empty and then filled: one call makes it, another uploads the bytes. Both can fail, and creation returns `null` on failure.

## Hint 2

Create with `SDL_PIXELFORMAT_ABGR8888`, `SDL_TEXTUREACCESS_STATIC`, `SIZE`, `SIZE`. Upload `&pixels[0] as const void*` with pitch `SIZE * 4`. In `draw`, build an `SDL_FRect` and pass it as the destination to `SDL_RenderTexture`.

## Hint 3

```dc
sdl3::SDL_Texture* texture = sdl3::SDL_CreateTexture(renderer, sdl3::SDL_PixelFormat::SDL_PIXELFORMAT_ABGR8888,
                                                     sdl3::SDL_TextureAccess::SDL_TEXTUREACCESS_STATIC, SIZE, SIZE);
```

then `if texture == null { return null; }`, fill the array, `SDL_UpdateTexture(texture, null, ..., SIZE * 4)`, `SDL_SetTextureScaleMode(...)` and `return texture;`. In `draw`, finish with `SDL_RenderTexture(renderer, texture, null, &dst)`.
