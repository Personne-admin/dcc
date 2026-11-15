## Hint 1

The two kinds of number must meet as `f64`. Convert the integer side with
`as f64` before it takes part in a decimal calculation.

## Hint 2

`total` adds the three variables. `whole_average` divides `total` by 3
directly. For `average` and `cost`, write `total as f64` and `cups as f64`,
and divide or multiply with an `f64` such as `3.0` or `price`.

## Hint 3

```dc
i32 total = first + second + third;
i32 whole_average = total / 3;
f64 average = total as f64 / 3.0;
f64 cost = price * cups as f64;
```
