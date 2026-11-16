# Optimize: a comparison budget

`find_sorted` looks for a key in a sorted slice and returns its index. The starter scans from
the front, so a search in 1000 values may need 1000 comparisons. Time is a poor way to measure
that on a fast machine, so the project has `src/counter.dc`: every comparison goes through
`counter::compare(a, b)` (-1, 0 or 1), and the harness counts the calls.

```text
find_sorted([0, 2, 4, ..., 1998], 998)  ->  Some(499)
find_sorted([0, 2, 4, ..., 1998], 999)  ->  None
```

## Your task

Make `find_sorted` in `src/search.dc` correct for every key and use at most **11 comparisons**
for a slice of 1000 values (and at most 1 for a slice of 1 value). Keep calling
`counter::compare` for every comparison of a value with the key; do not edit `counter.dc`.

The sorted data has no duplicates. `src/main.dc` prints the index and the number of
comparisons for three searches in 1000 values; the harness checks the budget.
