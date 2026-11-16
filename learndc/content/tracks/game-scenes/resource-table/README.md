# A resource table

A game that draws a wall, a hero and a key needs three textures that live and die together. This exercise puts them in a
small struct and writes the two functions of their whole lifetime: `load` and `unload`.

Edit `src/resources.dc`. The picture data and `art::make` (it turns pixel art into a texture and returns `null`
when the art is malformed, for example rows of different lengths) are in `src/art.dc`; `main` is finished.

## unload

`unload(r)` destroys every texture in the table that exists and sets its field back to `null`:

```dc
if r.key != null {
    sdl3::SDL_DestroyTexture(r.key);
    r.key = null;
}
```

Do this for all three fields. Nothing happens for a field that is already `null`, so `unload` can be called on an empty
table, twice in a row, or after a failed `load`.

## load_with

`load_with(r, renderer, wall_art, hero_art, key_art)` creates the three textures with `art::make`, in the order wall, hero,
key, and stores them in the table. It returns `true` when all three exist. If any `make` returns `null`, it must **release
what it already created** by calling `unload(r)` and return `false`; the caller then never has to guess what is half loaded.

`load(r, renderer)` is the version `main` uses; it passes the real art. The checks call `load_with` with a malformed
picture in each position, so every failing path is exercised.

## main

`main` creates the table zeroed (`Resources res = {};`), loads it, writes `defer resources::unload(&res);` on the next line
and draws the three textures. The destroys run before the renderer is destroyed, because the `defer` of the renderer was
written earlier.

## What you should see

Three enlarged sprites in a row on a dark gray window: a brick wall, the hero and a key. `learndc run` opens the window;
Esc quits.

The checks use an offscreen window. They verify the table's fields and the pixels that SDL's software renderer draws from
the textures. They cannot count how many textures SDL still holds, so whether you destroy each one is something to
confirm by reading your own code.
