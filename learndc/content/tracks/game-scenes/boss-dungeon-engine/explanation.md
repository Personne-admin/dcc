# Why this works

The engine is built in layers, and every layer can be tested without the one above it. `level` turns text into typed tiles and
rejects broken plans at the boundary, so nothing later parses characters. `collision` knows only boxes and solid tiles. `game` is
the state and the rules: plain structs and functions of `Input`, with no SDL in sight, which is why the scenes, the keys and
the doors are checked by scripted games. `resources` and `render` are the only modules that touch SDL, and `main` only connects
the keyboard and the clock to them.

The scene machine is the lecture's pattern with real content. One enum, one `match` in `update` and one in `draw`, entry work
done where the transition happens (`start` resets everything, so the second game is the first game again) and presses detected
through the stored previous input. Pause is a scene, not a flag, so "the world is frozen" follows from the arm doing nothing.

Interaction adds the idea that tiles are mutable state: a picked up key and an opened door are not special objects, they are
the tile becoming `Floor`. That also makes restarting trivial, because `start` reloads the plan and every key and door is back.

The resource table is the same contract as the exercise, with six handles. `load_with` is all or nothing, `unload` releases only
what exists and clears every field, and `main` owns the table with one `defer` written right after the successful load and after the
renderer's defers, so the textures go before the renderer and every path out of `main`, the early returns included, is covered.
The old `main` in stage 1 does the opposite (all cleanup in one place at the bottom) and shows what an early return costs.

Animation is a pure function of the accumulated play time: the hero's frame is `(elapsed_ms / 200) % 2`, so it runs at the same pace
at any frame rate and pauses for free while the elapsed time stops growing.
