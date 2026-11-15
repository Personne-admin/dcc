# A week of steps

A step counter recorded how many steps you walked on each of five days. The
five numbers live in one fixed array:

```dc
const usize DAYS = 5;
i32[DAYS] steps = { 4000, 6500, 3000, 8000, 5500 };
```

`DAYS` names the length once. Use it in the declaration and in every loop,
so the program stays correct if the number ever changes.

- `steps[0]` is the first value, `steps[DAYS - 1]` the last.
- `steps[1] = 7200;` writes a new value into one box.
- `for i in 0..DAYS { ... }` visits every valid index, from 0 to `DAYS - 1`.
  The loop variable `i` is a `usize`, which is also the type of an index.

## Your task

`src/main.dc` declares the array and nothing else. Complete the five tasks
written in its comments, in order:

1. Day 2 was counted wrongly. Change it to 7200.
2. Print one line per day, numbered from 1.
3. Add up every day and print the total.
4. Print the value of the last day by using `DAYS - 1` as the index.
5. Find the day with the most steps and print its number (counted from 1).

For the last task keep a variable `best` with the index of the best day so
far. Start it at 0 and replace it whenever a later value is larger.

## Acceptance criteria

- The project builds with `dpm build`.
- Running it prints exactly:

```text
day 1: 4000
day 2: 7200
day 3: 3000
day 4: 8000
day 5: 5500
total: 27700
last day: 5500
best day: 4
```
