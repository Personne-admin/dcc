## Hint 1

Every shape follows the same three steps: set the color, build an `SDL_FRect` or choose coordinates, call the drawing function. Do them in the order of the table.

## Hint 2

`set_color(renderer, r, g, b)` changes the color. `SDL_FRect ground = {x = 0.0, y = 110.0, w = 200.0, h = 40.0};` and `sdl3::SDL_RenderFillRect(renderer, &ground);` draw the ground. The window frame uses `SDL_RenderRect`, the roof two `SDL_RenderLine` calls, the birds `SDL_RenderPoint`.

## Hint 3

```
set_color(renderer, 110, 170, 230);
sdl3::SDL_RenderClear(renderer);
sdl3::SDL_FRect ground = {x = 0.0, y = 110.0, w = 200.0, h = 40.0};
set_color(renderer, 60, 150, 70);
sdl3::SDL_RenderFillRect(renderer, &ground);
```
Repeat the pattern for the body, door and frame. A line is `sdl3::SDL_RenderLine(renderer, 55.0, 60.0, 100.0, 15.0);`.
