# Sort and search a scoreboard

You have written loops that count, search and compare. Most of those jobs
already exist in the library, tested and fast. Import the module and call
the function by its qualified name:

```dc
import std::sort;
import std::slice;

std::sort::sort(scores);                    // ascending, in place
std::slice::reverse(scores);                // flip the order, in place
std::slice::count(scores, 17);              // how many equal 17
std::slice::index_of_max(scores);           // Optional(usize): first largest
std::sort::binary_search(sorted, 65);       // Optional(usize): needs ascending order
```

All of them take a slice. A mutable slice (`[]i32`) lets the function
change the elements; a `[]const i32` only lets it read them. Searches that
can come up empty return `Optional(usize)`: `unwrap_some()` gives the index
when `is_some()` is true.

`binary_search` halves the range at every step, so it only works on a slice
that is already sorted ascending. On any other order it simply answers wrong.

## Your task

`src/board.dc` has five stubs and `src/main.dc` already calls them. Implement
them with the library; each body is one or two lines.

1. `leader(scores)`: the index of the largest score (`index_of_max`, and the
   slice is never empty).
2. `times(scores, score)`: how many scores equal `score`.
3. `rank_desc(scores)`: sort the slice from high to low, in place.
4. `median(scores)`: sort the slice ascending in place and return the middle
   element, `scores[scores.len / 2]`.
5. `find_score(sorted, score)`: the index of `score` in an ascending slice,
   or `Optional::None`.

`median` really does sort the caller's slice, so by the time `main` searches,
the scores are ascending again. The program prints:

```text
leader at index 2
17 appears 2 times
ranked: 93 93 65 42 30 17 17 8
median 42
65 is at index 5
50 is missing
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The report above matches exactly and the exit code is 0.
- The harness checks pass, including negative scores and repeated maxima.
