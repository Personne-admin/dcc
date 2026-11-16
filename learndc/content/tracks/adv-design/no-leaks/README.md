# Fix the leaking path

`src/lengths.dc` has one generic function, `unique_lengths(alloc, words)`. It
returns the distinct word lengths in ascending order as an array owned by the
caller. For ten words it prints

```text
word lengths
distinct lengths: 1 3 4 5
words: 10, distinct lengths: 4
```

and it already does, but it only works when no allocation fails. It builds two
arrays through the allocator it is given:

- `all` collects the length of every word and is sorted. It is a
  temporary and must always be released before the function returns.
- `result` collects the distinct lengths. On success it is handed to the
  caller, who releases it. On failure the function must release it.

Every `push` can fail with `AllocError` and returns early through `?`. As written,
`all.deinit()` sits at the end of the function and `result` is never released
on an error path, so a failure leaks.

The harness runs the function with an allocator of its own that counts
allocations and frees and fails the Nth allocation, for every N the function
makes (more than a dozen for a long word list). Each run must return an error
and each run must end with as many frees as allocations.

## Your task

Fix `unique_lengths` so that every failure point is clean. Keep the
signature and the result unchanged:

1. Register the cleanup of `all` with `defer` right after it is created.
2. Give `result` a hand-over flag and a deferred block that frees it only if
   the flag is still false.
3. Set the flag after the last fallible step, just before `return`.

`main.dc` uses the heap and must stay unchanged.

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The report above matches exactly and the exit code is 0.
- The harness sweeps a short and a long word list and every failure point
  leaves no block behind.
