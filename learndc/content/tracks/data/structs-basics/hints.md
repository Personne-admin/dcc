## Hint 1

`new_player` is one `return` with a struct literal. The literal names every
field: `name`, `health` and `score`. The other functions read and change the
fields of their parameter `p` with `p.health`, `p.score` and so on.

## Hint 2

`award` and `hit` change a field of `p` and then `return p;` so the caller
gets the changed copy. For `hit`, subtract first and then use an `if` to put
a negative health back to 0. `is_alive` is a single comparison.

## Hint 3

```dc
public Player new_player([]const char name) {
    return Player { name = name, health = 100, score = 0 };
}
```

`award` does `p.score += points;`. `hit` does `p.health -= damage;`, then
`if p.health < 0 { p.health = 0; }`. `is_alive` returns `p.health > 0`.
