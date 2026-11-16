## Hint 1

Declarations are private unless they start with `public`. Which three functions are only called from inside the module?

## Hint 2

The helpers are used by `mean` and `max` in the same file, which is allowed for private functions. Remove the keyword from `sum_of`, `count_of` and `is_empty_set`.

## Hint 3

Change `public i64 sum_of` to `i64 sum_of`, and the same for `usize count_of` and `bool is_empty_set`.
