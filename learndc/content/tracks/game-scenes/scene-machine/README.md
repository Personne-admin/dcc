# A scene machine

Build the skeleton of a game that has four scenes: **Title**, **Play**, **Pause** and **Game over**. There is no
sprite and no map yet; the point is the structure that every bigger game in this chapter uses.

Edit `src/game.dc` (the logic) and `src/render.dc` (the drawing). `main.dc` is finished: it turns the keyboard into an
`Input` and calls `update` and `draw` once per frame.

## The rules (update)

`update(g, in)` receives the keys that are *down* right now. Detect presses (down now, not down in the previous frame)
using `g.prev`, the previous `Input` that the game keeps. Then act on the scene:

| Scene | Press | Effect |
|---|---|---|
| Title | `confirm` | start: scene Play, score 0, lives 3 |
| Play | `hit` | score + 1 |
| Play | `hurt` | lives - 1 |
| Play | `pause` | scene Pause |
| Pause | `pause` | scene Play. Hit and hurt do nothing here |
| Game over | `confirm` | scene Title |

When lives reach 0 in Play the scene becomes Game over, even if `pause` is pressed in the same frame. At the end of
**every** call, whatever the scene, store the input: `g.prev = in;`.

## The pictures (draw)

`render.dc` already draws title, play and game over (`draw_title`, `draw_play`, `draw_game_over`). Write:

- `draw_pause(renderer, g)`: the play scene underneath (call `draw_play`), then two light bars (230, 230, 230) on top:
  rectangles at (130, 90) and (170, 90), each 20 wide and 60 high.
- `draw(renderer, g)`: a `match` on `g.scene` that calls the right function for each scene.

## Try it

`learndc run` opens a 320 by 240 window. Enter confirms, Space hits, H hurts, P pauses, Esc quits. Hold a key
for a while: a press counts once, which is the whole reason for `g.prev`.

The first two checks play scripted games through `update` with no window. The third renders each scene on an
offscreen window and reads pixels back; it checks the drawing code, not an on-screen window.
