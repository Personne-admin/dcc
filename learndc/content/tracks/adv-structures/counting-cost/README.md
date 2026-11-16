# Counting cost

"This sort is fast" is not a measurement a program can check, and the time a
run takes depends on the machine, the load and the data. To compare two
algorithms, count what they do: how many comparisons, how many moves, how many
allocations. A count is the same on every computer and every day, it is
easy to assert in a test, and it shows how the work grows when the input
grows.

## A counter you pass around

Give the algorithm a small struct to count in, and increment it where the
expensive operation happens:

```dc example=search_cost
```

- `less` and `equal` do the comparison and add one to the count. Every
  comparison of the search goes through them, so the number is exact.
- The linear search looks at items one after another. Its cost is the
  position of the key, or all 1024 when the key is missing.
- The binary search keeps a range `low..high` and halves it with each
  comparison. 1024 items need 10 halvings and one final equality test: 11
  comparisons for any key, present or not.
- Binary search needs the data sorted. That is its entry price; the next
  lessons show what the sorting costs.

## How the work grows

Doubling the input is the question. A linear search does twice as many
comparisons; a binary search does one more. Written with the usual notation:

| growth | name | when n doubles |
|---|---|---|
| about log2(n) | logarithmic | one more step |
| about n | linear | twice the work |
| about n * log2(n) | n log n | slightly more than twice |
| about n * n | quadratic | four times the work |

The notation drops constants and keeps only the shape. For small inputs a
quadratic algorithm with a small constant can win, and for a data set
of 10 items nothing matters. The shape is what predicts what happens at
10,000 or a million items.

## Cost depends on the input

The same algorithm can behave very differently on different data:

```dc example=sort_cost
```

- Insertion sort moves each item left until it meets a smaller one. On sorted
  data that is one comparison per item (255 for 256 items). On reversed data
  every item moves all the way: 255 + 254 + ... + 1 = 32640, which is
  n * (n - 1) / 2. On random data it is about a quarter of n * n.
- Merge sort halves the runs and merges them: about n * log2(n) comparisons
  whatever the order, 1024 for 256 sorted items and up to n log n for the
  rest.
- Neither is "the fast one". Insertion sort is excellent when the data is
  nearly sorted or tiny, merge sort when nothing is known. Library sorts
  combine the two for exactly this reason.

## What to count

Pick the operation that dominates the real cost: comparisons for searching and
sorting, node visits for a linked list, allocations for a container that
grows, bytes copied for a buffer. A test that asserts "at most 12 comparisons
for 1000 items" fails when someone replaces binary search with a linear loop,
which no output test would notice.

## Common mistakes

- Timing a tiny input and concluding that two algorithms are the same.
- Counting only the successful path. A search that fails is often the
  expensive one.
- Comparing counts from different data. Use the same input for both
  algorithms, and look at best, worst and typical data.
- Forgetting a hidden cost: binary search on an unsorted array finds
  nothing reliable, and sorting before every search can cost more than a
  linear search.
- Counting in a global variable. Pass the counter explicitly, so two
  searches in the same program do not share it.
