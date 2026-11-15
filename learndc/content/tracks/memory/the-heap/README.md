# Owning memory: Array and the heap

A fixed array such as `i32[5]` has its size in its type and lives right
inside the scope that declares it. It can never hold a sixth element. This
lecture is about the growable kind, and about the responsibility that comes
with it.

## Fixed size versus growable

`Array(T)` keeps its elements in a block of memory from the *heap*: memory
the program requests from the system while it runs, in whatever size it
needs. The `Array(T)` variable itself is small. It holds a slice of that
block (an address and how many elements fit) and a count of the elements in
use:

```text
scores (in main)                      the heap
+---------------------------+        +----+----+----+----+-- ... --+
| storage: address, room 8  | -----> | 70 | 85 | 92 |    |         |
| count:   3                |        +----+----+----+----+-- ... --+
+---------------------------+
```

When `count` reaches the room in the block, `push` asks for a bigger block,
copies the elements over and gives the old block back. That is why the
elements can move.

## Creating, growing, releasing

```dc example=array_grow
```

- `{}` makes an empty array. It holds no heap memory yet.
- `push` adds one element at the end. It may need a bigger block, and asking
  for memory can fail, so it returns a status and you write `?`.
- `len()` is the number of elements, and `items()` lends you all of them as
  a slice that a `for` loop can walk.
- `deinit()` gives the block back. `defer` on the next line makes sure that
  happens exactly once, on every way out of `main`.

## Who owns it?

The variable that created the array *owns* its memory and is the one that
must release it. DC does not track this for you, so keep the rule simple:

- One owner per array, and the owner writes `defer x.deinit();` right after
  creating it.
- Copying the variable (`Array(i32) other = scores;`) copies the small
  handle, not the elements. Both now point at the same block, but only one of
  them may release it. Avoid such copies.
- A slice from `items()` is borrowed. It is valid until the array changes
  size; after a `push` or `remove`, ask for a fresh slice.

## Lending an array to a function

A function that should add to the caller's array takes a pointer,
`Array(i32)* out`, and the caller passes the address with `&numbers`. The
function borrows the array; the caller still owns it and still releases it.

```dc example=fill_by_pointer
```

Through the pointer, `out.push(n)` works exactly as on the array itself.
A function that only reads takes `const Array(i32)*`, which promises not to
change it.

## Common mistakes

- Forgetting `deinit`: the program works, and leaks the block.
- Releasing twice through two copies of the same array.
- Keeping a slice from `items()` across a `push`.
- Returning `items()` of a local array from a function: the deferred
  `deinit` gives the memory back before the caller can read it.
