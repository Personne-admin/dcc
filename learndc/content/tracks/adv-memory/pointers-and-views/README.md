# Pointers and views

A pointer is a number: the address of a value. This lesson is about the
arithmetic you can do with that number, and about `core::slice::from_raw`,
the bridge that turns an address and a count back into a slice.

## Walking with a pointer

`&x` takes the address of `x`; `*p` reads or writes the value at `p`. When
`p` points into an array you can also move it. Adding `n` to a pointer moves
it by `n` *elements*, that is by `n * sizeof(T)` bytes: DC never makes you
multiply by the size yourself.

```dc example=walk
```

- `p++`, `p += 3`, `p - 1` and `p + n` give pointers `n` elements away.
- `p[i]` means `*(p + i)`. It reads like an array, but it is plain
  arithmetic.
- Subtracting two pointers of the same type gives the number of elements
  between them, as a signed `isize`.
- Pointers compare with `==`, `!=`, `<` and so on. Comparing is only
  meaningful for pointers into the same block.

None of this is checked. `p[10]` on a pointer into a five-element array
compiles and reads whatever bytes happen to be there. A pointer knows an
address and a type; it does not know where its array ends.

## A search that returns a pointer

A pointer is a good answer to "where is it?": the address itself, or `null`
when there is none. A function that receives a pointer and a count walks
from `p` to `p + n`, and `p + n` is the *end*: one element past the last, an
address you may compare with but never read.

```dc example=find
```

`null` is the pointer that points at nothing. Always test for it before you
use `*`.

## From pointer and count to a slice

A pointer plus a count is exactly what a slice stores. `core::slice::from_raw`
builds the slice from the two; `from_raw_const` does the same for a pointer to
`const` data and gives a `[]const T`.

```dc example=views
```

The slice does not copy anything. `middle` and `data` are two names for the
same bytes, which is why `middle[0] = 30` changes `data[2]`. This is how an
allocator hands out memory: it finds a block of bytes, and returns a slice of
the right length that points into it.

`from_raw` trusts you completely. It cannot check that `count` elements
really exist at that address. Getting that right is your job.

## Why there is no `p[1..3]`

On a pointer, a range index does not compile:

```text
error: cannot range-slice a pointer
```

A range such as `1..3` must be checked against the length of the thing that
is sliced, and a pointer has no length. Slice an array or a slice, or state
the count yourself with `from_raw`.

## Dangling views

A slice or pointer to a local variable stops being valid when the function
that owns the variable returns. The same is true after the owner released a
block (see the allocator lesson). Nothing in the program changes; the bytes
just belong to somebody else, and reading them is undefined behaviour.

Keep the rule from the memory chapter: a view is borrowed. Find out who owns
the bytes, and make sure the owner lives longer than every view.

## Common mistakes

- Reading or writing at `p + n`, the end address. It is only for comparing.
- Forgetting the count: a function that takes only `i32* p` cannot know how
  many elements follow. Pass a slice, or a pointer and a count.
- Counting in bytes. `p + 8` is eight elements, not eight bytes.
- Returning `from_raw(&local[0], 3)` of a local array.
- Mixing `const i32*` and `i32*` in a comparison or a subtraction: both are
  rejected. Copy one into a variable of the other's type first.
