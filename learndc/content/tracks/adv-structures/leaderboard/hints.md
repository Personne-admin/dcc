## Hint 1

Insertion sort takes each item from the second one on and moves it left past
the items that must come after it. The only thing that changes between
sorts is the question "must this one come after that one?", and that is the
comparator.

## Hint 2

Shift while `j > 0 && compare(&items[j - 1], &value) > 0`. With a strict
`> 0` an equal neighbour stops the shifting, so equal items keep their order.
`by_score_then_name` compares scores in reverse first and returns
`std::mem::compare(a.name, b.name)` when they are equal. In `assign_ranks`
the rank at index `i` is `i + 1`, unless the score equals the previous
player's, in which case it copies the previous rank.

## Hint 3

```
for usize i in 0..sorted.len {
    if i > 0 && sorted[i].score == sorted[i - 1].score {
        ranks[i] = ranks[i - 1];
    } else {
        ranks[i] = (i + 1) as u32;
    }
}
```
