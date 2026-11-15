# Arithmetic with whole numbers

DC calculates with the usual operators:

| Operator | Meaning | Example | Result |
|---|---|---|---|
| `+` | add | `7 + 2` | 9 |
| `-` | subtract | `7 - 2` | 5 |
| `*` | multiply | `7 * 2` | 14 |
| `/` | divide | `7 / 2` | 3 |
| `%` | remainder | `7 % 2` | 1 |

Division of whole numbers has no fraction: `7 / 2` is `3`, the part after
the point is simply dropped. The `%` operator gives what was left over, so
the two belong together: 7 is 3 groups of 2 with 1 left over.

```dc
i32 total = 17;
i32 groups = total / 5;
i32 rest = total % 5;
```

Here `groups` is 3 and `rest` is 2, because 3 * 5 + 2 is 17.

Multiplication and division happen before addition and subtraction, like in
school. Parentheses make a part happen first:

```dc
i32 a = 2 + 3 * 4;
i32 b = (2 + 3) * 4;
```

`a` is 14 and `b` is 20.

## Your task

`src/main.dc` declares the variables but leaves them at 0. Give each of them
its calculation, so the program prints:

```text
135 minutes = 2 h 15 min
100 shared by 7 = 14 each, 2 left over
(8 + 4) * 3 = 36
8 + 4 * 3 = 20
```

- 135 minutes are `total_minutes / 60` hours and `total_minutes % 60`
  minutes.
- Sharing `bill` among `friends` gives `each` with `left_over` coins that
  cannot be split.
- `grouped` and `plain` use the same numbers 8, 4 and 3; only the parentheses
  differ.

Change the declarations only, not the `println` lines.

## Acceptance criteria

- The project builds.
- The program prints exactly the four lines above and exits with code 0.
