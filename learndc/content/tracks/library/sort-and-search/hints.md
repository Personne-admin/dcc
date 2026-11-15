## Hint 1

`board.dc` already imports `std::slice` and `std::sort`. Each stub has a
matching library function, so you only have to pick the right one and pass
the slice on.

## Hint 2

Call the functions with their module path: `std::slice::count(scores, score)`
for `times`, `std::slice::index_of_max(scores)` for `leader` (it returns an
`Optional(usize)`, so finish with `.unwrap_some()`) and
`std::sort::binary_search(sorted, score)` for `find_score`. For `rank_desc`
sort first, then reverse.

## Hint 3

```dc
public usize leader([]const i32 scores) {
    return std::slice::index_of_max(scores).unwrap_some();
}

public void rank_desc([]i32 scores) {
    std::sort::sort(scores);
    std::slice::reverse(scores);
}

public i32 median([]i32 scores) {
    std::sort::sort(scores);
    return scores[scores.len / 2];
}
```

`times` returns `std::slice::count(scores, score)` and `find_score` returns
`std::sort::binary_search(sorted, score)` directly.
