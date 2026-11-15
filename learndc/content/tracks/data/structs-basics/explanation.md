# Why the solution works

- The struct literal in `new_player` is the only place that decides what a
  fresh player looks like. Every field is named, so the order of the fields
  does not matter and a forgotten field is a compile error rather than a
  silent zero.
- `award` and `hit` receive their own copy of the player. Changing
  `p.score` changes that copy only, which is why they `return p;` and why
  `main` writes `hero = player::hit(hero, 30);`. The check in the harness
  that the original player stays unchanged proves the copy semantics.
- `hit` first subtracts and then repairs the result, so a hit of 200 on a
  player with 70 health ends at 0, not at -130. Keeping a rule such as
  "health is never negative" inside the one function that changes health
  means the rest of the program can rely on it.
- `is_alive` only reads, so it does not need to return the player.

A struct has no behavior of its own: the functions that work on it are
ordinary functions placed next to it in the same module.
