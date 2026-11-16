# The Dungeon Engine

A tile-based dungeon game, assembled from everything in this path: a level written as text, a player that cannot walk
through walls, keys that open doors, four scenes, animated sprites and six textures that are all put back at the end.
The engine is half built; you finish it stage by stage.

## The game

The dungeon is 16 by 12 tiles of 20 pixels. Two keys (`k`) lie in the rooms, two doors (`D`) lock the way, and the
exit (`E`) is in the last room. The visitor walks with the arrow keys, picks keys up by walking over them and opens a
door by standing next to it and pressing Space (**use**); each door costs one key. Touching the exit after collecting
both keys wins. Enter confirms in the menus, P pauses, Esc quits.

```text
################
#@...#.....#..E#
#....#..k..#...#
#....D.....D...#
#....#.....#...#
#.k..#.....#...#
...
```

## The modules (all in `src/`)

| File | State | What you write |
|---|---|---|
| `level.dc` | tiles, the plan `LEVEL` | `load`, `tile_at`, `solid` |
| `collision.dc` | finished (the box and tile test from the earlier exercise, now asking `level::solid`) | nothing |
| `game.dc` | scenes, input, `start` is finished | `walk`, `interact`, `update` |
| `resources.dc` | the table of six textures | `load_with`, `unload` |
| `art.dc` | pixel art and `art::make` | nothing |
| `render.dc` | `fill` helper | `draw` |
| `main.dc` | window, input, loop, cleanup | nothing |

## The stages

1. **Read the legacy main.** Two questions about the keeper's old `main`: find what it leaks and where the fixed `defer` goes.
2. **Load the dungeon.** `level::load` fills a `Level` from rows of text, counts the keys, finds the start and rejects
   broken plans. `tile_at` answers `Tile::Wall` outside the grid; `solid` is true for walls and doors.
3. **Walk and bump.** `game::walk(g, in, dt)` moves the player by `SPEED * dt` pixels in the held directions, through one
   `collision::move` call.
4. **Keys, doors and the exit.** `game::interact(g, in)`: stepping on a key picks it up (`keys` and `collected` grow, the tile
   becomes floor); a press of `use` opens one directly neighbouring door and spends a key; the exit sets `exit_reached`
   when `collected` equals the level's key count.
5. **Scenes.** `game::update(g, in, dt)`: Title (confirm starts), Play (pause press pauses; otherwise count time, walk, interact,
   win), Pause (pause press resumes, nothing moves), Win (confirm returns to the title). Detect presses with `g.prev` and
   store the input at the end.
6. **Own every texture.** `resources::load_with` creates the six textures all or nothing; `unload` is repeatable.
7. **Draw the dungeon.** `render::draw(renderer, g, res)` per scene. Floor under every tile that is not a wall, then wall, door,
   key or exit on top, then the hero. The hero texture holds two 8 by 8 frames and the frame is `(elapsed_ms / 200) % 2`. The title,
   pause and win screens are made of filled rectangles; the exact numbers are in the stage hints.
8. **Bonus: the key counter.** A key icon per held key in the top left corner.

## Try it

Once the stages pass, `learndc run` plays the game: title, dungeon, pause, win and back to the title. Esc quits.

The checks play scripted games through `update`, `walk` and `interact` with no window, and render every scene on an
offscreen window with SDL's software renderer and read pixels back. They cannot judge how the movement feels, how
smooth the animation is, or whether a window opens on your desktop. Whether every texture is destroyed cannot be
measured either; the unload and failure checks verify the table's fields, and the legacy stage asks you to reason about
the rest.
