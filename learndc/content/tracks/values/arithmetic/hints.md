## Hint 1

Dividing whole numbers drops the fraction. `/` gives how many full groups
fit, and `%` gives what is left after them.

## Hint 2

Replace each `= 0;` with a calculation using the variables above it:
`hours` and `each` use `/`, `minutes` and `left_over` use `%`. For the last
two, write the sums with the literal numbers 8, 4 and 3.

## Hint 3

```dc
i32 hours = total_minutes / 60;
i32 minutes = total_minutes % 60;
i32 grouped = (8 + 4) * 3;
i32 plain = 8 + 4 * 3;
```

`each` and `left_over` follow the same pattern with `bill` and `friends`.
