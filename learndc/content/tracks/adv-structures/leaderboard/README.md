# A stable leaderboard

`src/board.dc` has the pieces of a scoreboard. Some are written, three are
placeholders:

- `insertion_sort_by(items, compare)` must sort any slice with a comparator
  of type `i32(*)(const T*, const T*)`. It must be **stable**: items that
  compare equal keep their order.
- `by_score_then_name(a, b)` must order players by score, highest first, and
  players with the same score by name in alphabetical order. (`by_name` and
  `by_score_desc` are provided.)
- `assign_ranks(sorted, ranks)` writes the rank of each player of an already
  sorted slice into `ranks`. Players with the same score share a rank, and the
  next rank skips the tied places: scores 95, 95, 80, 80, 80, 70 get the
  ranks 1, 1, 3, 3, 3, 6.

`main.dc` sorts the same eight players with your sort and with `std::sort`,
prints the ranked table and compares the two results. Then it sorts the
players in arrival order by score only, to show that your sort keeps ties in
arrival order.

```text
leaderboard
1 kim 95
1 otto 95
3 ada 80
3 bo 80
3 mira 80
6 lena 70
7 ivo 60
7 zed 60
same as std::sort: true
score order, ties by arrival: otto kim mira ada bo lena zed ivo
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The output above matches exactly and the exit code is 0.
- The harness checks stability against `std::sort::sort_stable_by` on
  records with many equal keys, and the comparator and ranks on other data.
