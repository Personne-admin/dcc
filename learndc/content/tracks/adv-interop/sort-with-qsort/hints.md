## Hint 1

All seven functions are short. Start with the two comparators: cast both
arguments (`a as const Player*`), then compare one field with `<` and `>`.
Everything else passes them on to the C function.

## Hint 2

`qsort(players as void*, count, sizeof(Player), by_score_desc);` is the whole
body of `sort_by_score`. For `bsearch` build `Player key = Player { id = id,
score = 0 };` and pass `&key as const void*`. For `qsort_r` write a private
callback with the three-argument signature that casts `context as Counter*`,
increments `calls`, and returns `by_score_desc(a, b)`.

## Hint 3

`by_score_desc`: return -1 when `x.score > y.score`, 1 when it is smaller, then
break ties on the id the same way, and finally return 0. `find_by_id` ends
with `return hit as Player*;`, where `hit` is the `void*` that bsearch
returned.
