# Stacks, queues and linked structures

A data structure is a contract about order: which element comes out next,
and what each operation costs. This lesson builds the two classic orders, last
in first out (a stack) and first in first out (a queue), in three layouts, and
then looks at which the library gives you.

## A stack is an Array used at one end

Pushing and popping at the end of an `Array` is cheap: `push` writes the
next slot (the array doubles its block when it is full, so a push costs a
constant amount on average), and `pop` just decreases the count. A stack is
the natural tool for anything nested, such as matching brackets:

```dc example=brackets
```

- Each opening bracket is pushed. A closing bracket must match the top.
- The input is balanced only if every closer found its opener and the stack is
  empty at the end.
- `defer open.deinit();` releases the array on every `return`.

## A queue at the wrong end is expensive

A queue removes from the front. With an `Array` that means `remove(0)`,
which moves every other element one place left: the cost is the length of
the queue, for every single pop. Draining a queue of n items by `remove(0)`
costs about n * n / 2 moves. A *ring buffer* avoids moving anything. It keeps
a fixed array, the index of the oldest item (`head`) and the count:

```dc example=ring
```

- `tail = (head + len) % CAPACITY` is the slot after the last item; it
  wraps around the end of the array.
- Pop moves `head` forward instead of moving data. Both operations cost a
  few instructions, whatever the size.
- The ring never allocates, and it is full when `len == CAPACITY`. A
  full push is refused, not grown: the capacity is part of the design (a log
  window, an input queue with a bound, a producer that must slow down).
- The capacity is a named constant because a struct template takes types,
  not values.

## Linked nodes and ownership

A linked structure allocates one node per item, and each node points to
the next. Insertion and removal at a known place touch two pointers. In
return every item costs an allocation and the nodes are scattered in memory,
so walking the list is slower than walking an array. The key difference is
*ownership*: the structure owns its nodes, and every node must be freed
exactly once.

```dc example=linked
```

- `alloc.create_item()` and `alloc.destroy_item(node)` take a node from and
  give it back to the allocator. `Counting` wraps a fixed-buffer allocator and
  counts the calls, so the program can show that the numbers match.
- `push` can fail, because allocating can fail; it returns `Status(AllocError)`
  and a failed push leaves the stack unchanged, so nothing leaks.
- `pop` unlinks the node before it frees it and copies the value out first.
  Reading `node.next` or `node.value` after `destroy_item` is a use after free.
- `defer clear(&stack, &alloc);` frees whatever is still linked on every way
  out. A second `clear` is harmless, because it frees nothing the second time.
- When allocations and frees do not match at the end of a run, the structure
  leaks (or frees twice). A counting allocator in a test turns that into a
  number.

A queue is the same list with a `tail` pointer: push at the tail, pop at the
head, both in constant time. You will write one in the next exercise.

## The library: when to use which

```dc example=std_list
```

`std::list` is an *intrusive* doubly linked list: the nodes carry `prev`
and `next`, the list holds `head`, `tail` and a count. `push_back_alloc` and
`push_front_alloc` allocate nodes for you; `pop_front` and `pop_back` unlink a
node and hand it back to you, and you free it with `destroy_node` (or
`destroy_item`). `deinit(&alloc)` frees every remaining node.

| structure | strengths | costs |
|---|---|---|
| `Array(T)` | contiguous, fast to walk, index access, amortized O(1) push and pop at the end | insert or remove at the front or middle moves elements; elements can move when it grows |
| `Map(K, V)` | O(1) on average for put, get, remove by key (open addressing, load factor 3/4) | no order; growth rehashes; the key type needs a hash |
| `std::list::List(T)` | O(1) insert or unlink next to a node you hold; node addresses never change | one allocation per item; no indexing; slow to traverse |
| ring buffer | O(1) both ends, no allocation, bounded memory | fixed capacity |

Start with `Array`. Reach for a ring when the queue must be bounded and
allocation-free, a `Map` when the question is "do I know this key", a list
when you need stable addresses or constant-time unlinking in the middle.

## Common mistakes

- Using `remove(0)` on an `Array` as a queue.
- Reading a node after freeing it: copy `value` and `next` first.
- Freeing a node while it is still linked, or linking one twice.
- Losing the only pointer to a node (the allocation leaks): overwriting
  `head` before saving its `next`.
- Forgetting the `tail` pointer in a queue: when the last node is popped,
  set `tail` to `null`, or the next push writes through a freed node.
- Using a ring's `len == CAPACITY` as "empty": `len == 0` is empty.
