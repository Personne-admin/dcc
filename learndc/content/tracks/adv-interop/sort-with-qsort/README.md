# Sort and search with libc

The C library sorts and searches any array if you give it three facts and a
function: where the array starts, how many elements it has, how big one
element is, and how to compare two of them. `src/board.dc` already declares
`qsort`, `bsearch` and `qsort_r` and a `Player` struct (an `id` and a
`score`). You write seven small functions around them.

## The comparators

Both take `const void*` and cast back with `as const Player*`.

- `by_score_desc` sorts the *highest* score first. Players with the same
  score are ordered by the smaller `id`, so the order is fully defined.
- `by_id` sorts by ascending `id`.

Return a negative number, 0 or a positive number. Compare with `<` and `>`;
do not return a difference, which can overflow.

## The wrappers

- `sort_by_score(players, count)` and `sort_by_id(players, count)` call
  `qsort` with `sizeof(Player)` as the element size.
- `find_by_id(players, count, id)` uses `bsearch` on an array that is already
  sorted by id. The key is a `Player` with the wanted id (the score is
  ignored by `by_id`). `bsearch` returns a `void*` to the element or `null`;
  return it as a `Player*`.
- `sort_counting(players, count)` sorts by score like `sort_by_score`, but
  with `qsort_r`, whose callback receives an extra `void*` context. Give it a
  small struct holding a counter, increase the counter in the callback, and
  return the number of comparisons.

`src/main.dc` is finished and prints a report:

```text
by score:
  3 90
  5 90
  1 65
  7 40
  9 10
by id:
  1 65
  3 90
  5 90
  7 40
  9 10
found 5: score 90
4 not found
counted comparisons: true
```

The pointer `find_by_id` returns points *into* the array, so changing
`found.score` changes the array itself. A second check tests the comparators
with extreme values and the wrappers with arrays of size 0, 1 and 6.
