# Game state and scenes

A game with a title screen, a pause menu and a game over screen is not four programs. It is one program that is
always in exactly one of a few modes. This lecture shows how to model those modes as data so that the code stays
small and testable.

All examples run without a window: the logic of a game does not need SDL.

## State is data

Everything the game needs to remember between two frames belongs in one struct: the score, the lives, where the
player is. Two rules keep it manageable.

1. `update(Game*, Input)` changes the state and never draws. It takes the keys as a plain struct of booleans, not
   SDL events, so a test can feed it any sequence of inputs.
2. `draw(renderer, Game*)` reads the state and never changes it.

The state is the single source of truth: if you can print the `Game`, you know exactly what the screen should show.
This is why the earlier chapters kept `game.dc` free of SDL calls, and why this chapter's checks can play a
whole game in a few lines.

## Scenes as an enum

The mode is a field of the state, and an enum is the natural type for "one of a few things":

```dc example=scene_basics
```

`update` is a `match` on the scene. Because a `match` must be exhaustive, adding a fifth scene later makes the
compiler point at every `match` that does not handle it yet. A bunch of `bool in_menu; bool paused; bool dead;`
flags cannot do that, and they allow impossible combinations such as "paused and dead".

## Transitions

A transition is nothing more than assigning a new value to `g.scene`. Two habits keep them tidy:

- Do the *entry work* where you make the transition. Starting a game resets score and lives at the moment the
  scene changes from Title to Play, not in some other place that must remember to do it.
- Decide which transition wins when two conditions are true in the same frame. Above, losing the last life beats
  pressing pause, and the check is written in that order.

## Pressed or held?

A key is *down* for many frames, but a menu choice should happen once per press. Sampling the keyboard every
frame gives you *held*. To get *pressed* you remember last frame's value and look for the change:

```dc example=edge_input
```

Without the edge, holding Enter on the title screen would start the game and, a frame later, also "confirm" inside
the play scene. Store the previous value in the state, compare, and update it at the end of **every** `update`,
whatever scene you are in; if one early `return` skips the update, the next frame sees a stale value and a press fires
twice.

## One draw per scene

Drawing mirrors updating: a `match` on the scene that calls one function per scene. A pause screen usually shows the
play scene frozen underneath, so `draw_pause` calls `draw_play` first and adds its own bars on top. Scenes can share
drawing code freely because drawing never changes the state.

## Common mistakes

- Calling SDL from `update`. The logic can no longer be tested without a window.
- Using several bools instead of one enum, and allowing states that make no sense.
- Using the held value where the press is meant: menus flicker through several screens.
- Forgetting to reset the state on entering Play, so the second game starts with the old score.
- Forgetting to store the previous input on every path.
