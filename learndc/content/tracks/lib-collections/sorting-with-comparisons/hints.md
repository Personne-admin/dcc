## Hint 1

A comparison function never returns the sign of a subtraction of unsigned numbers (u32 would wrap). Compare with `>` and `<` and return -1, 1 or 0.

## Hint 2

`by_score_desc`: if `a.score > b.score` return -1, if `a.score < b.score` return 1, else 0. `rank`: create `Array(Player) scratch = {};`, `defer scratch.deinit();`, `scratch.resize(players.len())?;`, then call `std::sort::sort_stable_by(players.items(), scratch.items(), by_score_desc);`.

## Hint 3

`by_name`: `return compare_text(a.name, b.name);`. `find_by_name`: `Player key = Player { name = name, score = 0 };` then `return std::sort::binary_search_by(sorted, &key, by_name);`.
