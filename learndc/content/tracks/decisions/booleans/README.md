# True and false

A `bool` holds one of two values, `true` or `false`. Every comparison
(`==`, `!=`, `<`, `<=`, `>`, `>=`) produces one, so a function that answers a
yes/no question can simply return the comparison:

```dc
public bool can_vote(i32 age) {
    return age >= 18;
}
```

Printing a `bool` with `{}` shows `true` or `false`. A `bool` can be stored
in a variable like any other value: `bool adult = age >= 18;`.

## Combining conditions

Three operators build bigger conditions out of smaller ones:

| expression | meaning                    | true when                         |
| ---------- | -------------------------- | --------------------------------- |
| `a && b`   | a and b                    | both are true                     |
| `a \|\| b` | a or b                     | at least one is true              |
| `!a`       | not a                      | a is false                        |

```dc
bool teenager = age >= 13 && age <= 19;
bool odd_one = day == 1 || day == 15;
bool not_empty = !(count == 0);
```

Write both comparisons in full: `age >= 13 && age <= 19` is right, while
`13 <= age <= 19` is not how a range check is written.

## Precedence and parentheses

`!` is evaluated first, then `&&`, then `||`. That is why
`true || false && false` is `true`: the `&&` part happens first. When a
condition mixes `&&` and `||`, add parentheses so a reader (and the compiler)
agree on the grouping:

```dc
year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
```

`&&` and `||` also stop early: when the left side already decides the answer,
the right side is not evaluated. `d != 0 && 10 / d > 1` never divides by zero,
because `d != 0` is false and ends the condition.

## Your task

`src/rules.dc` has five functions that return `false`. Implement them:

| function                       | returns true when                                   |
| ------------------------------ | --------------------------------------------------- |
| `can_vote(age)`                | `age` is 18 or more                                 |
| `in_range(n, low, high)`       | `n` is between `low` and `high`, both ends included |
| `is_weekend(day)`              | `day` is 6 or 7 (days are numbered 1, Monday, to 7) |
| `is_working_day(day)`          | `day` is from 1 to 7 and is not a weekend; reuse `in_range` and `is_weekend` |
| `is_leap_year(year)`           | `year` is divisible by 4, except years divisible by 100, unless they are also divisible by 400 |

`src/main.dc` prints some answers; leave it as it is. For example 1900 is not
a leap year (divisible by 100 but not 400), while 2000 is.

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches and both function checks pass, including the boundary
  values 17 and 18, the ends of the range and the days 0 and 8.
