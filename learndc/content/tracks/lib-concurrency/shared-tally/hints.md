## Hint 1

All three functions have the same skeleton: `tally.lock.lock();` then `defer tally.lock.unlock();` on the very next line, then the actual work.

## Hint 2

In `try_add`, the refusal path is `if tally.words + amount > tally.limit { return false; }` inside the locked region. The deferred unlock still runs on that return. After the check, `tally.words += amount;` and `return true;`.

## Hint 3

`words_so_far`: lock, `defer` unlock, `return tally.words;`. The return value is computed before the deferred statement runs, so it is safe to return a field of locked data.
