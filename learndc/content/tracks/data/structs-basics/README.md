# Your first struct

A game needs to remember a player's name, health and score. Three separate
variables work for one player and fall apart for two: which `health` belongs
to whom? A *struct* gathers values that belong together into one type with
named fields.

```dc
public struct Player {
    []const char name;
    i32 health;
    i32 score;
}
```

`Player` is now a type like `i32`. A *struct literal* builds a value and
names every field:

```dc
Player hero = Player { name = "Ada", health = 100, score = 0 };
println("{} has {} health", hero.name, hero.health)?;
hero.score = 10;
```

- `hero.health` reads a field, `hero.score = 10;` changes one.
- A literal must set every field; leaving one out is a compile error
  (`missing initializer for field ...`).
- A struct is a value, just like an `i32`. Assigning it to another variable
  or passing it to a function **copies** it.

## Functions that take and return structs

Because a parameter is a copy, changing it inside the function does not
touch the caller's value. To hand a changed player back, return it:

```dc
public Player award(Player p, i32 points) {
    p.score += points;
    return p;
}

hero = award(hero, 50);      // store the returned copy
```

If the caller forgets `hero = ...`, the change is simply thrown away. The
next lesson shows another way to change the caller's value directly.

## Your task

`src/player.dc` declares `Player` and four stubs. `src/main.dc` is finished
and prints the player after each event. Implement:

- `new_player(name)` returns a player with that name, 100 health and 0 score.
- `award(p, points)` returns the player with `points` added to the score.
- `hit(p, damage)` returns the player with `damage` taken off the health.
  Health never drops below 0.
- `is_alive(p)` is true while the health is above 0.

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- Running it prints exactly:

```text
Ada: health 100, score 0
Ada: health 70, score 50
alive: true
Ada: health 0, score 50
alive: false
```

- The harness also checks that the player you pass in is left unchanged.
