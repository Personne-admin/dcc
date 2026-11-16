# Textures and sprites

Rectangles are enough for Pong. A dungeon needs a hero, walls and keys, and
those are small pictures: *sprites*. This lecture explains where a picture
lives in SDL3, how the bytes of a picture are laid out, how to turn pixel art
written as text into one, and how to draw only part of it.

The examples on these pages are plain DC programs that run without a window:
they do the byte and rectangle arithmetic that every sprite needs. The SDL
calls themselves are shown in code blocks and practised in the exercises of
this chapter, where checks render the result and read the pixels back.

## Surfaces and textures

SDL3 has two kinds of picture, and the difference is *where the pixels live*.

- A **surface** (`SDL_Surface*`) is a picture in ordinary CPU memory. It is a
  struct with `w`, `h`, `pitch`, `format` and a `pixels` pointer you can read
  and write like any array. Surfaces are what image files load into
  (`SDL_LoadBMP`) and what you build by hand (`SDL_CreateSurface`).
- A **texture** (`SDL_Texture*`) is a picture that belongs to a *renderer*. It
  is an opaque handle: the pixels may sit in graphics-card memory, so there is
  no `pixels` pointer. You can replace its pixels (`SDL_UpdateTexture`) but the
  renderer is the only one that draws it (`SDL_RenderTexture`).

```text
CPU memory                          renderer memory (often the GPU)
+----------------+   copy once     +----------------+
| SDL_Surface    | --------------> | SDL_Texture    | --> window, every frame
| pixels you own |                 | drawn quickly  |
+----------------+                 +----------------+
```

A game keeps its sprites as textures because drawing a texture is cheap, and
copying pixels from CPU memory every frame is not. You build the pixels once,
upload them once, and then only say *where* to draw them.

Both kinds are handles you own. `SDL_CreateTexture` and
`SDL_CreateTextureFromSurface` need a matching `SDL_DestroyTexture`;
`SDL_CreateSurface` and `SDL_LoadBMP` need `SDL_DestroySurface`. A texture
belongs to the renderer that made it and must be destroyed before that
renderer.

## Pixels in memory

A picture is a block of bytes: `height` rows, each holding `width` pixels.
In the format used throughout this chapter every pixel takes four bytes, in
the order **red, green, blue, alpha** (alpha 255 is solid, 0 is invisible).

```dc example=pixel_layout
```

Two numbers describe the block. The *width* is pixels per row. The *pitch* is
**bytes** per row. With no padding the pitch is `width * 4`, but SDL is
allowed to pad rows, which is why every function that takes pixels also takes
a pitch instead of guessing it. The byte where pixel (x, y) starts is
`y * pitch + x * 4`.

The format is named by an enum value. SDL names packed formats by the bits of
one 32-bit number, written from the highest bits down, and a little-endian
machine stores the lowest byte first. So the bytes in memory read R, G, B, A
for the format called `SDL_PIXELFORMAT_ABGR8888`. SDL also spells this
`SDL_PIXELFORMAT_RGBA32` ("the bytes are R, G, B, A, whatever the machine"),
but the DC bindings declare only the numbered names, so you will write
`sdl3::SDL_PixelFormat::SDL_PIXELFORMAT_ABGR8888`.

## Pixel art as text

You do not need an image editor to make a sprite. A grid of characters is
pixel art: one character per pixel and a function that says what each
character means.

```dc example=art_to_pixels
```

`color_for` is the palette and `put` writes one pixel at its place. Keeping
those two apart from SDL has a practical benefit: you can test the decoding
without a window, which is exactly what the exercise checks do. `&pixels[0]`
passes the address of the first byte so `put` can write through a pointer.

## Giving pixels to SDL

With bytes in memory, a texture is three calls. This code is shown, not run
here; you will write it in the next exercise.

```dc
sdl3::SDL_Texture* tex = sdl3::SDL_CreateTexture(
    renderer,
    sdl3::SDL_PixelFormat::SDL_PIXELFORMAT_ABGR8888,
    sdl3::SDL_TextureAccess::SDL_TEXTUREACCESS_STATIC,
    5, 5);
if tex == null {
    return null;
}
sdl3::SDL_UpdateTexture(tex, null, &pixels[0] as const void*, 5 * 4);
sdl3::SDL_SetTextureScaleMode(tex, sdl3::SDL_ScaleMode::SDL_SCALEMODE_NEAREST);
```

`STATIC` access means "uploaded now and then, drawn often", the right choice
for sprites. The `null` rectangle in `SDL_UpdateTexture` means the whole
texture, and the last argument is the pitch of *your* bytes. Every SDL call
that can fail returns `null` or `false`; `sdl3::SDL_GetError()` describes why.

## Source and destination rectangles

`SDL_RenderTexture(renderer, tex, src, dst)` copies a rectangle of the texture
(`src`, in texture pixels) onto a rectangle of the screen (`dst`, in window
pixels). A `null` source is the whole texture. The sizes need not match:
drawing a 5 by 5 sprite into a 40 by 40 rectangle scales it up eightfold.

A **sprite sheet** keeps many frames in one texture. Choosing a frame is only
choosing the source rectangle, and that is plain arithmetic:

```dc example=frame_rects
```

One texture with many frames beats many textures: fewer handles to own and
destroy, and animation costs nothing but a different `src`.

## Crisp pixels and ownership

Scaling a small sprite smooths it by default (linear filtering), which turns
pixel art into a blur. `SDL_SetTextureScaleMode` with
`SDL_SCALEMODE_NEAREST` copies the nearest source pixel instead, so every
sprite pixel becomes a sharp square.

The other rule is lifetime. Textures are resources, and the earlier chapters
already told you what to do with resources: write the release right after the
acquire, with `defer`.

```dc
sdl3::SDL_Texture* hero = make_hero(renderer);
if hero == null {
    return 3;
}
defer sdl3::SDL_DestroyTexture(hero);
```

Declared after the renderer's `defer`, the texture is destroyed first, which
is the order SDL needs.

## Common mistakes

- Passing the wrong pitch. `width * 4` bytes per row, not `width`. A wrong
  pitch shears the picture or reads past your array.
- Forgetting the null check on a created texture, then drawing with it. SDL
  reports the problem through `SDL_GetError`, not with a crash.
- Mixing up `src` and `dst`. `src` is in texture pixels, `dst` in window
  pixels; `null` for `src` means everything, for `dst` the whole window.
- Leaving the default linear filter on a small sprite and wondering why the
  pixel art is blurry.
- Destroying the renderer before the textures made from it.
- Treating `RGBA` and `ABGR` as different byte orders. For the format used
  here the bytes are R, G, B, A.
