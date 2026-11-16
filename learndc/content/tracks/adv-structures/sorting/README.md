# Sorting

Sorting turns "find the biggest", "group the equals" and "search in
logarithmic time" into simple loops. This lesson writes one sort by hand to
see how it works and what it costs, then uses the library sorts in
`std::sort` for real work, with comparators and with a stability guarantee.

## One sort by hand

Insertion sort keeps the front of the slice sorted. For each new item it
shifts the larger items one place to the right and drops the item into the
gap:

```dc example=insertion
```

- `[]T items` is a mutable slice, so the sort changes the caller's array.
- The comparison is not hard-coded. `compare` is a function pointer of type
  `i32(*)(const T*, const T*)`: it returns a negative number when the first
  item belongs before the second, zero when they are equal and a positive
  number when it belongs after. Passing `descending` reverses the result of
  the same algorithm.
- `compare(&items[j - 1], &value) > 0` shifts only items that are strictly
  greater. Equal items stay where they are, which makes the sort *stable*
  (see below).
- The cost is the one you counted: n - 1 comparisons for sorted data and
  about n * n / 2 for reversed data.

## The library sorts

`std::sort` has the same shape as your function, with more care:

| function | what it gives |
|---|---|
| `sort(items)` | sorts by `<` for any type with `<`; unstable |
| `sort_by(items, compare)` | the same with your comparator; unstable |
| `sort_stable_by(items, scratch, compare)` | stable merge sort; needs a scratch slice as long as `items` |
| `sort_stable_alloc(&allocator, items, compare)` | stable, allocates the scratch for you and returns a status |
| `is_sorted_by(items, compare)` | true if no item is after its successor |

`sort` and `sort_by` are an introsort: a quicksort that falls back to
a heap sort when it recurses too deep and finishes small pieces (24 items or
fewer) with insertion sort. The worst case is therefore n log n, not n * n.
The functions are generic, so each item type gets its own compiled copy.

## Comparators with several keys

A comparator is ordinary code, so it can order by several keys. Compare the
first key; only when it is equal, compare the next:

```dc example=comparators
```

- The score is compared in reverse (higher first), the name is the
  tiebreaker. Because every pair of entries is ordered by the two keys, the
  output is fully determined.
- `std::mem::compare` returns -1, 0 or 1 for two texts.
- A comparator must be consistent: if `a` is before `b` and `b` before `c`,
  `a` must be before `c`. A comparator that is not consistent can make a sort
  place items wrongly without failing.

## Stable sorting

A sort is *stable* if items that compare equal keep their original relative
order. That matters when the data is sorted twice (first by name, then by
score) or when equal items carry different information, such as the arrival
order of tasks:

```dc example=stable
```

- Within priority 1 the tasks stay in the order 2, 4, 7 in which they
  arrived; within priority 2, 1, 3, 6.
- `sort` and `sort_by` make no such promise. When the comparator orders
  every pair completely, stability does not matter; when it does not, use
  `sort_stable_by` or add a tiebreaker such as the original index to the
  comparator.
- A stable sort needs extra memory. `sort_stable_by` takes the scratch slice
  from you, so you decide where it lives; `sort_stable_alloc` takes an
  allocator and releases the scratch before it returns.

## Searching what you sorted

`std::sort::binary_search(items, key)` returns `Optional(usize)`, and
`lower_bound_by` / `upper_bound_by` take the same comparators as `sort_by`.
The search only works on data sorted with the same comparator: sort and
search must agree on the order.

## Common mistakes

- Writing `>= 0` in the shifting test of insertion sort. Equal items then
  move past each other and the sort is no longer stable.
- Passing a comparator for one order to a search over data in another.
- Sorting a copy of the handle instead of the data. A slice is a view, so
  `std::sort::sort(view)` sorts the array it views.
- Using a stable sort without a scratch slice that is as long as the data.
- Writing a comparator that returns a `bool`. It returns a negative, zero or
  positive `i32`.
