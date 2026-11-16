## Hint 1

`open` is a chain of steps where each can fail. After every step ask: what did the earlier steps create that I must undo before returning false?

## Hint 2

The order is Init, window, renderer. A failing window needs only `SDL_Quit()`. A failing renderer needs `SDL_DestroyWindow(window)` and then `SDL_Quit()`. `close` is the same list in reverse, guarded by `null` checks.

## Hint 3

```
if !sdl3::SDL_Init(sdl3::SDL_INIT_VIDEO) { return false; }
sdl3::SDL_Window* window = sdl3::SDL_CreateWindow(title, width, height, 0);
if window == null { sdl3::SDL_Quit(); return false; }
sdl3::SDL_Renderer* renderer = sdl3::SDL_CreateRenderer(window, renderer_name);
if renderer == null { sdl3::SDL_DestroyWindow(window); sdl3::SDL_Quit(); return false; }
g.window = window;
g.renderer = renderer;
return true;
```
