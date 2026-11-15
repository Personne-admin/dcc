# Changing a value step by step

A variable can change while the program runs. You already know plain
assignment:

```dc
coins = coins + 30;
```

The right side is calculated first, using the old value, and the result goes
back into the variable. Updating a variable from itself is so common that DC
has short forms:

| Short form | Same as |
|---|---|
| `coins += 30;` | `coins = coins + 30;` |
| `coins -= 15;` | `coins = coins - 15;` |
| `coins *= 2;` | `coins = coins * 2;` |
| `coins /= 2;` | `coins = coins / 2;` |
| `coins++;` | `coins = coins + 1;` |

A value that must never change gets `const` in front. The compiler then
refuses any later assignment, so nobody can change it by accident:

```dc
const i32 SWEET = 5;
coins -= SWEET;
```

Writing `SWEET = 6;` afterwards is an error: `cannot assign to const-qualified
target`. Constants are named in capitals by convention.

## Your task

`src/main.dc` starts with 20 coins and prints the count after every event,
but nothing changes the variable yet. Under each comment, write the statement
for that event so the program prints:

```text
start: 20
birthday: 50
book: 35
doubled: 70
found: 71
sweet: 66
```

Use the short forms and use `SWEET` for the last step. Do not change the
`println` lines.

## Acceptance criteria

- The project builds.
- The program prints exactly the six lines above and exits with code 0.
