# Why it works

The renderer is a state machine. `SDL_SetRenderDrawColor` stores the current color; `SDL_RenderClear`, `SDL_RenderFillRect`, `SDL_RenderLine` and the rest paint with whatever color is current. Nothing is visible until `SDL_RenderPresent`, which `main` calls once per frame after `draw` finished.

Because every call paints over what is already there, the order is the layering. The body is drawn after the ground and the door after the body, so the door is visible. Swap two calls and the later one hides the earlier one. The outlined window frame works the other way round: `SDL_RenderRect` draws only the border, so the body color stays inside it.

Coordinates have their origin at the top left and y grows downwards, which is why the ground (y 110 to 150) is near the bottom of the window and the roof apex (y 15) is near the top. A filled rectangle covers `x` to `x + w - 1`; the checks look at the first and last pixel on each edge to confirm that.

Lines include both end points, so a diagonal at 45 degrees touches the pixel `(80, 35)` exactly; this is why the roof uses 45 degree slopes.
