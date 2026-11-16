# The second largest value

`second_largest` returns the largest value that is strictly smaller than the maximum. Duplicates
of the maximum do not count as a second value.

```text
[4, 9, 2, 9, 7]   Some(7)
[5, 5, 5]         None
[3]               None
[]                None
[-4, -9]          Some(-9)
```

## Your task

Implement `public Optional(i32) second_largest([]const i32 values)` in `src/ranking.dc`.
Make one pass over the data; do not sort and do not change the slice.

`src/main.dc` prints the answers for a few lists and must print:

```text
[4, 9, 2, 9, 7]: 7
[5, 5, 5]: none
[3]: none
[-4, -9]: -9
```
