# Reading a run-length encoder

## What to look for

Run-length encoding replaces `aaab` with `3a1b`: a count and a character for every run of equal characters. It is the simplest compression scheme and a good exercise in reading loops.

Two things to watch while reading: the loops that scan the text (which variable moves where?) and the ownership of the array of runs (who creates it, who fills it, who frees it?).

## The program

```dc example=rle
```

## Reading it

- `main` owns the `Array(Run)`: it declares it and defers `deinit`. `encode`, `show` and `decode` only borrow it through `Array(Run)*`.
- `encode` has two loops. The outer `while i < text.len` handles one run per round: it remembers the symbol, then the inner `while` advances `i` and counts as long as the same symbol continues. When the inner loop ends, `i` is on the first character of the next run, so the outer loop continues from there without any extra step.
- `runs.push(...)?` can fail (memory), so `encode` returns a `Status` and passes the failure on with `?`.
- `decode` is the mirror image: for each run it prints the symbol `length` times.

Check the boundaries: an empty text never enters the outer loop and produces no runs; a text of one character produces one run of length 1.

## Common mistakes

- Advancing `i` in the outer loop as well as in the inner one: runs would be skipped.
- Forgetting the `i < text.len` test in the inner loop: it would read past the end of the text.
- Resetting `length` outside the outer loop: counts would add up across runs.
- Releasing the array in a helper. Only the owner, `main`, calls `deinit`.
