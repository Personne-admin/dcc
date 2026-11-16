# A generic min-heap

`src/heap.dc` holds a priority queue stored in an `Array(T)` and ordered by
a comparator `i32(*)(const T*, const T*)`. The first item of the array is
always the one that comes first by the comparator. The functions are
placeholders; `heap_push` only appends.

- `heap_push(heap, value, compare)`: append the value and sift it up.
  Returns the `Status(AllocError)` of the append.
- `heap_pop(heap, compare)`: remove and return the first item, or `None` for
  an empty heap. Move the last item to the root, shrink the array, sift down.
- `heap_peek(heap)`: the first item without removing it, or `None`.
- `is_heap(heap, compare)`: true when no item comes before its parent.

Indexes: the parent of `i` is `(i - 1) / 2`, the children are `2 * i + 1`
and `2 * i + 2`. A pop on an array with n items needs at most about
2 * log2(n) comparisons and a push at most log2(n); the harness counts them.

Name your helpers anything except `push` and `pop`: a function with these
names in the module would be called instead of `Array.push` and `Array.pop`
when you write `heap.push(value)`.

`main.dc` is a deadline queue of tasks. Its comparator orders by `due` and
then by arrival `order`, so equal deadlines come out first scheduled first.
It must print:

```text
deadline queue
waiting: 7, heap valid: true
next: deploy (due 5)
  5 deploy
 10 email
 10 standup
 20 report
 20 review
 25 lunch
 30 backup
empty pop is none: true
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The output above matches exactly and the exit code is 0.
- The harness checks the heap property after every push and pop, the sorted
  order of the pops and the number of comparisons.
