# Sort a leaderboard with comparisons

`std::sort::sort` orders numbers with `<`. A struct needs a rule: which
player comes first? You supply the rule as a **comparison function** and the
library does the sorting:

```dc
i32 by_score_desc(const Player* a, const Player* b) { ... }

std::sort::sort_by(players.items(), by_score_desc);
```

The function receives pointers to two elements and returns an `i32`:

- negative when `a` must come **before** `b`,
- positive when `b` must come before `a`,
- `0` when neither has to come first (they tie).

Two kinds of sort exist, and the difference matters for ties:

| Function | Ties | Extra memory |
| -------- | ---- | ------------ |
| `sort_by(items, cmp)` | may end up in any order (unstable) | none |
| `sort_stable_by(items, scratch, cmp)` | keep their input order (stable) | a scratch slice as long as `items` |

For the scratch slice, make an `Array` of the same element type and call
`scratch.resize(players.len())?`; `scratch.items()` is the slice. Release the
scratch array with `defer`.

Searching a sorted array uses the same comparison function:
`std::sort::binary_search_by(items, &key, cmp)` returns an `Optional(usize)`.
`key` is a value of the element type; only the fields the comparison looks at
matter.

## Your task

`src/main.dc` has a `Player` (name and score), a finished `compare_text`
that orders two texts, and four stubs. `main` is finished.

1. `by_score_desc` puts the **higher** score first and returns 0 for equal
   scores.
2. `by_name` orders by name with `compare_text`.
3. `rank` sorts the players with `by_score_desc` using a **stable** sort, so
   players with equal scores stay in roster order.
4. `find_by_name(sorted, name)` binary-searches an array sorted by name. Build
   a `Player` with that name (any score) as the key.

Expected output:

```text
ranking:
1. Dara 90
2. Bo 85
3. Eli 85
4. Ana 70
5. Cy 70
alphabetical: Ana Bo Cy Dara Eli
Eli is at index 4
Zed not found
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly; equal scores keep their roster order.
