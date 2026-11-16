## Hint 1

`update` is a `match` on `g.scene`. Before it, turn each held key into a press: `bool confirm = in.confirm && !g.prev.confirm;` and so on. At the very end, remember the input for the next frame.

## Hint 2

Play: add to the score on a hit press, take a life on a hurt press, then `if g.lives <= 0 { game over } else if pause { Pause }`. Title sets score and lives when it starts the game. In `render.dc`, `draw` is a `match` with one arm per `game::Scene` value, and `draw_pause` is `draw_play` plus two `fill` calls.

## Hint 3

```dc
match g.scene {
    Scene::Title => { if confirm { g.scene = Scene::Play; g.score = 0; g.lives = 3; } },
    Scene::Play => { ...hit, hurt, then the lives and pause test... },
    Scene::Pause => { if pause { g.scene = Scene::Play; } },
    Scene::GameOver => { if confirm { g.scene = Scene::Title; } },
}
g.prev = in;
```
