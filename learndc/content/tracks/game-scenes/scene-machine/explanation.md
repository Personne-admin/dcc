# Why this works

The game is a struct plus two functions, which is why the first two checks can play it without SDL: `update` takes plain
booleans and changes plain fields. The scene is an enum, so there is exactly one mode at a time, and the `match` forces
you to say what each mode does. Adding a fifth scene later would stop compiling until every `match` handles it.

A press is a change between two frames, so the state keeps the previous input. If the update of `g.prev` were inside one
branch, the other scenes would compare against a stale value and the key that started the game would also score a hit.
Putting it as the last statement of `update`, outside the match, covers every scene with one line.

The transitions do their entry work where they happen: the Title arm resets score and lives, so a second game starts fresh
no matter how the first one ended. In Play, the lives test comes before the pause test, which is a design decision (death
beats pause) written down as the order of an `if` chain.

Drawing mirrors updating: one `match`, one function per scene. `draw_pause` shows how scenes share drawing code: it is the
play scene frozen under two bars, so it calls `draw_play` instead of repeating it. This works because drawing only reads
the state.
