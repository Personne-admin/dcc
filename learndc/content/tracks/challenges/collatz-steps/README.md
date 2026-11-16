# Collatz steps and peaks

Start with any positive number `n`. If it is even, halve it; if it is odd, triple it and add 1.
Repeat until you reach 1. The number of repetitions is the number of steps, and the largest
number seen on the way (the start included) is the peak.

```text
6 -> 3 -> 10 -> 5 -> 16 -> 8 -> 4 -> 2 -> 1      8 steps, peak 16
```

## Your task

Implement both functions in `src/collatz.dc`:

- `public u64 steps(u64 n)`: the number of steps to reach 1 (0 for `n == 1`).
- `public u64 peak(u64 n)`: the highest value of the sequence, the start included.

`n` is at least 1. `src/main.dc` prints a small table and must print:

```text
6: 8 steps, peak 16
7: 16 steps, peak 52
27: 111 steps, peak 9232
```
