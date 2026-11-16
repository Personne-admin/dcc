# Why this works

One texture with many frames beats many textures: there is one handle to own and destroy, and choosing a frame costs
nothing but a different source rectangle. The rectangle is plain arithmetic on the index, so it belongs in a small function
you can test without SDL, which is why the first check never opens a window.

The destination rectangle is where scaling happens: it is the frame's size times the scale factor, so the same 8 by 8
frame covers 32 by 32 window pixels at scale 4. Because `create` selected nearest-neighbour scaling, every texel becomes a
sharp block and the source rectangle never samples the neighbouring frame.

`SDL_RenderTextureRotated` is the general form of `SDL_RenderTexture`: angle, center of rotation and flip as extra arguments.
With angle 0 and no center it is a plain copy, and a flip mirrors the frame inside the destination rectangle. Drawing a left-facing
character as a mirrored right-facing one halves the art you need. Rotation by other angles works the same way; a spinning
sprite is only an angle that grows with time.
