## Hint 1

Index 0 is day 1, so day 2 is `steps[1]`. A loop `for i in 0..DAYS` gives
you each index `i`; print `i + 1` for the day number and `steps[i]` for the
value.

## Hint 2

The total is a running sum: declare `i32 total = 0;` before a second loop and
write `total += steps[i];` inside it. The last day needs no loop at all:
`steps[DAYS - 1]`. For the best day, keep `usize best = 0;` and, in a loop
from index 1, replace it when `steps[i] > steps[best]`.

## Hint 3

```dc
steps[1] = 7200;
for i in 0..DAYS {
    println("day {}: {}", i + 1, steps[i])?;
}
```

Then a loop that does `total += steps[i];`, the line
`println("last day: {}", steps[DAYS - 1])?;` and a loop `for i in 1..DAYS`
with `if steps[i] > steps[best] { best = i; }`. Print `best + 1` at the end.
