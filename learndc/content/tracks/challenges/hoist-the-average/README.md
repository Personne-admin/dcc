# Optimize: do not recompute inside the loop

`count_above_average` counts the values that are strictly greater than the integer average of
the slice. The starter calls `average(values)` inside the loop condition, so for 100 values it
reads about 10,000 elements. All reads go through `probe::read`, and the harness counts them.

```text
count_above_average([1, 2, 3, 10, 12])  ->  2     (average 5)
```

## Your task

Change `count_above_average` in `src/above.dc` so that it makes at most `2 * values.len` calls
to `probe::read`, and keep every answer the same. An empty slice returns 0. Do not edit `probe.dc`.

`src/main.dc` prints the answer and the number of reads, and must print:

```text
above average: 2
reads: 10
```
