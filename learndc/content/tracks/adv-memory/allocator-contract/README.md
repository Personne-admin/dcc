# The allocator contract

`Array` grows by asking for memory. `Map` does the same. Neither of them
knows where the memory comes from: they ask an *allocator*, and the
standard library has several. This lesson shows the one contract they all
share, so that you can use any of them directly, and later write your own.

## Three methods

An allocator in DC is an ordinary struct with three methods, called on a
pointer to it:

```text
allocate(len, align)               -> Result([]u8, AllocError)
resize_in_place(block, len, align) -> bool
free(block, align)
```

- `allocate` returns a block of exactly `len` bytes whose address is a
  multiple of `align`, or an `AllocError`: `OutOfMemory`, or
  `InvalidAlignment` when `align` is not a power of two.
- `resize_in_place` tries to change the length of a block without moving it
  and says whether it worked.
- `free` gives a block back. You pass the block (its current length counts)
  and the alignment you asked for.

There is no table of function pointers and no hidden global: a generic
function that takes `A* alloc` is compiled for the concrete allocator type
it is given. `AllocatorPolicy(A)` is the constraint that checks the three
methods exist.

## The heap

`std::os::heap::Heap` is the allocator that asks the operating system. It
has no fields, so `Heap heap = {};` is all it takes to have one. It hands out
memory in whole pages, which is fine for the large block behind an `Array`
and wasteful for many small values: arenas exist for that case.

```dc example=heap_blocks
```

Three rules show up even in this small program:

- Every allocation can fail, so the result is a `Result` and you decide what
  to do with the error.
- You own the block until you free it. Nothing frees it for you.
- The alignment is part of the request. `align = 3` is refused.

## Typed helpers

Asking for bytes and casting is tedious. `std::alloc` adds helpers on top of
any allocator: `create_item` and `destroy_item` for one value,
`allocate_slice` and `free_slice` for several. They compute the size with
`sizeof` and the alignment with `alignof` for you.

```dc example=typed_helpers
```

One detail is easy to miss. The element type `T` is not written anywhere in
the call: the compiler infers it from the type the result is stored in. Either
declare the `Result` first, as above, or let `?` unwrap straight into a typed
variable:

```dc
Point* p = heap.create_item()?;
```

A chain such as `heap.create_item().unwrap()` gives the compiler nothing to infer
from and fails with "cannot deduce template argument `T`". The memory is
uninitialized: write every field before you read it.

## The fixed buffer

`FixedBuffer` is an allocator that owns no memory. You give it a slice of
bytes (here a local array) and it hands out pieces of it one after another:
the *bump* strategy. Allocation is only an addition, and it never calls the
operating system.

```dc example=fixed_buffer
```

- `FixedBuffer` holds the state. `borrow(&buffer)` makes a `FixedHandle`,
  the small value that carries the methods. The handle only points at the
  buffer, so the buffer must outlive it.
- When the next block does not fit, `allocate` returns `OutOfMemory`.
- `free` can only take back the *most recent* allocation. Everything else is
  just left in place until `reset` makes the whole buffer empty again.

That last rule is the price of being this simple. It is also why an arena
that is cleared as a whole is so cheap.

## Containers over any allocator

`Array(T)` is short for `Array(T, DefaultAllocator)`. The second parameter
is the allocator, and the array stores it by value. Because that stored
value is a copy, a policy that keeps its state inside itself would lose its
bookkeeping; the handle pattern (a small struct that points at the real
state) avoids that.

```dc example=array_in_fixed
```

The array keeps working until the buffer is exhausted, and then `push`
reports the failure like any other allocation error. `deinit` hands the
storage back to the same allocator that supplied it.

## Arenas

`Arena(P)` sits on top of another allocator `P`. It takes big chunks from `P`
and hands out small pieces of them. Individual `free` calls do nothing; the
whole arena is released at once by `reset` or `deinit`.

```dc example=std_arena
```

Use an arena when many values share a lifetime: everything allocated while
handling one request, or building one frame. You skip tracking each value and
free all of them with one call. In the boss fight you build one yourself.

## Who frees what

- Whoever calls `allocate` owns the block and must give it back to the
  *same* allocator, with the same alignment, exactly once.
- A container such as `Array` owns what it allocated; `deinit` releases it.
- An arena owns its chunks; pieces taken from it must not be freed on their
  own and must not be used after `reset`.

## Common mistakes

- Freeing a block with a different allocator than the one that allocated it.
- Using a block, or a slice into it, after `free`, `reset` or `deinit`.
- Reading memory from `allocate` before writing it.
- Forgetting that the typed helpers need a declared type to infer from.
- Keeping a `FixedHandle` or `ArenaHandle` after the buffer or arena it
  points to is gone.
