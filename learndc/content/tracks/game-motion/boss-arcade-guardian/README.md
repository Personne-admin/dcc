# The Arcade Guardian

A complete Pong game in a 640 x 360 window. Each stage is one part of the
rules and is checked on its own; clear them in order, and run `learndc run`
as soon as the first stages work: the game gets more complete with every
stage. A stage that passed earlier stays in your record, and the objective
list shows whether it still passes with your current code.

## What is already there

- `src/input.dc`: `Input` (held `w`, `s`, `up`, `down`; one-frame `restart`
  (Space or Return) and `toggle_ai` (Tab); `quit`) and `pump`, in the style of
  the previous chapter.
- `src/timing.dc`: the clamped `tick`.
- `src/runner.dc` and `src/main.dc`: the loop and the window. The loop reads
  input, measures `dt`, calls `pong::update`, then `render::draw`.
- `src/pong.dc`: the court constants, the `Paddle`, `Ball` and `Game` structs,
  `new_game(seed)`, the seeded generator `next_random(game)` (the one from
  *A seeded random number generator*), `axis`, and the finished
  `update(game, input, dt)`, which calls your functions in this order:

```text
toggle the AI if asked; if somebody has won, restart on request and stop here
left paddle:  move_paddle(left, axis(w, s), dt)
right paddle: ai_paddle (single player) or move_paddle(right, axis(up, down), dt)
step_ball -> hit_paddle left -> hit_paddle right -> score_if_out -> update_winner
```

- `src/render.dc`: `fill(renderer, x, y, w, h, r, g, b)` draws one rectangle,
  and a placeholder `draw`.

The court is `W` 640 by `H` 360. Paddles are 12 wide and 64 tall (`PADDLE_W`,
`PADDLE_H`) at `LEFT_X` 20 and `RIGHT_X` 608; `Paddle` stores only `y`, the top
edge. The ball is a 12 x 12 square whose `x`, `y` is its top left corner. You
write the functions in `pong.dc` and `render.dc`; the other files are done.

Controls: W and S move the left paddle; Up and Down move the right paddle when
the AI is switched off (Tab); Space or Return restarts after a win; Esc or
closing the window quits.

## Stage 1: Move the paddles

`move_paddle(p, dir, dt)`: `p.y += dir * PADDLE_SPEED * dt`, then keep `p.y`
in `0.0 ..= H - PADDLE_H`. `dir` is -1.0, 0.0 or 1.0 from the keys, or a
fraction from the AI.

## Stage 2: A ball between two walls

`step_ball(b, dt)`: move by the velocity, and bounce off the top and the bottom
only. The limits for `b.y` are `0.0` and `H - BALL`; mirror the overshoot and
flip `vy`, as in the bouncing-ball lesson. The left and right sides stay open:
a ball that leaves there is a point (stage 4).

## Stage 3: Bounce off a paddle

`hit_paddle(b, p, side)` returns whether the ball hit, and if so changes it.
`side` is 1 for the left paddle (at `LEFT_X`) and 2 for the right (at `RIGHT_X`).

1. It is a hit only if the ball moves **toward** the paddle (side 1: `vx < 0`;
   side 2: `vx > 0`) **and** the rectangles overlap (strict `<`, as in the
   overlap lesson). A ball that is already leaving is never hit again: that
   is what stops it from sticking to the paddle.
2. Push the ball out of the paddle: for the left paddle `b.x = LEFT_X +
   PADDLE_W`, for the right one `b.x = RIGHT_X - BALL`.
3. Flip `vx`.
4. The angle comes from where the ball hit: `offset = (ball center y - paddle
   center y) / (PADDLE_H / 2)`, limited to -1.0 ..= 1.0, and `vy = offset *
   MAX_VY`. A hit on the paddle's center sends the ball straight, a hit at its
   edge sends it at the steepest angle, up or down.
5. Call `speed_up(b)` (the bonus stage; it does nothing until you write it).

## Stage 4: Serve, score, win and restart

- `serve(g, dir)` puts the ball in the center, `x = (W - BALL) / 2`,
  `y = (H - BALL) / 2`, with `vx = dir * BALL_SPEED` (`dir` is 1 or -1) and
  a random vertical speed from the game's generator:
  `vy = (((next_random(g) >> 16) % 301) as f32) - 150.0`, between -150 and 150.
  Exactly one `next_random` call, so that the same seed always serves the same
  way.
- `score_if_out(g)` returns 0, 1 or 2. When the ball is completely past the
  left edge (`g.ball.x + BALL < 0.0`) the right player scores: add a point,
  `serve(g, -1)` (toward the player who lost the point) and return 2. Past the
  right edge (`g.ball.x > W`) the left player scores: add a point, `serve(g,
  1)` and return 1.
- `update_winner(g)` sets `g.winner` to 1 or 2 when that player's score has
  reached `WIN_SCORE` (5).
- `restart(g)` sets both scores and the winner to 0, puts both paddles at
  `(H - PADDLE_H) / 2.0` and serves toward the right.

## Stage 5: An opponent for one player

`ai_paddle(g, dt)` moves the right paddle toward the ball. Compare the ball's
center (`g.ball.y + BALL / 2.0`) with the paddle's center. If the ball is more
than 10 pixels below, move down; more than 10 above, move up; otherwise stay.
That *dead zone* keeps the paddle from trembling. Move with
`move_paddle(&g.right, dir * (AI_SPEED / PADDLE_SPEED), dt)`: the AI is slower
(220 pixels per second against 320) and the court limits apply for free.

## Stage 6: Draw the court

`draw(renderer, game)`, back to front:

| Part | Geometry | Color |
|---|---|---|
| background | clear | 16, 16, 24 |
| net | 18 dashes: x 318, y `i * 20 + 5`, 4 wide, 10 tall | 60, 60, 80 |
| left paddle | `LEFT_X`, `game.left.y`, `PADDLE_W` x `PADDLE_H` | 70, 150, 230 |
| right paddle | `RIGHT_X`, `game.right.y`, same size | 230, 120, 40 |
| ball | `game.ball.x`, `game.ball.y`, `BALL` x `BALL` | 255, 255, 255 |
| left pips | one 10 x 10 square per point at x `300 - 16 * (i + 1)`, y 12 | 70, 150, 230 |
| right pips | one per point at x `330 + 16 * i`, y 12 | 230, 120, 40 |
| winner banner | winner 1 or 2: x 160, y 150, 320 x 60, last | the winner's color |

## Stage 7: Play a whole match

This check drives `update` itself for a scripted match: the left paddle
wiggles on a fixed pattern against the AI, 120 steps per second, until
somebody has five points. It asserts that the match ends with a proper winner,
that the ball and the paddles never left the court, and that Restart and the AI
toggle work. It exercises your stages together, so a failure here usually points
back at an earlier stage.

## Bonus: the ball speeds up

`speed_up(b)` multiplies `vx` by 1.05, keeping `|vx|` at most `MAX_BALL_SPEED`
(600). `hit_paddle` already calls it. Faster rallies make the AI lose.

## Polish (not checked)

Ideas: a "point" flash, a trail behind the ball, a countdown before each serve,
ball speed reset on serve, a second ball.

## Run it

`learndc run` needs a desktop session. The checks verify the rules as plain
functions, the pixels of `draw` on an offscreen renderer and a whole scripted
match; they do not press keys in a real window. Beat the Guardian yourself:
that is the final test. Tab switches to two players.
