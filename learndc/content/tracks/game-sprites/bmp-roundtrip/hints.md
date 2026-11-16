## Hint 1

Three functions, one rule: whatever you create and do not return, you destroy. Creating a surface, saving it and destroying it are three steps of one function.

## Hint 2

Right after `if surface == null { return ...; }` write `defer sdl3::SDL_DestroySurface(surface);`. Then every `return` after it is safe. In `make_gradient` you return the surface, so there is no defer there; in `save_gradient` and `load_texture` the surface is only a temporary.

## Hint 3

```dc
sdl3::SDL_Surface* surface = sdl3::SDL_LoadBMP(path);
if surface == null { return null; }
defer sdl3::SDL_DestroySurface(surface);
sdl3::SDL_Texture* texture = sdl3::SDL_CreateTextureFromSurface(renderer, surface);
if texture == null { return null; }
sdl3::SDL_SetTextureScaleMode(texture, sdl3::SDL_ScaleMode::SDL_SCALEMODE_NEAREST);
return texture;
```
