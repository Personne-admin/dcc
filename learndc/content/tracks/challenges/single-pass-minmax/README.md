# Optimize: one pass for minimum and maximum

`min_max` returns the smallest and largest value of a non-empty slice. The starter loops twice,
once for each answer, so it reads every element twice. Reading is expensive in this
project: all reads go through `probe::read(values, i)`, and the harness counts them.

```text
min_max([4, 9, 2, 9, 7])  ->  min 2, max 9
```

## Your task

Change `min_max` in `src/stats.dc` so that it reads every element **exactly once**: at most
`values.len` calls to `probe::read`. The slice is never empty. Do not edit `probe.dc`.

`src/main.dc` prints the result and the number of reads, and must print:

```text
min 2, max 9
reads: 5
```
