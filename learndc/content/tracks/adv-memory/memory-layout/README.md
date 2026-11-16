# How values sit in memory

Memory is a long row of bytes, and every value occupies some of them. Most of
the time you never think about it. This chapter is about the times you do:
writing an allocator, passing data to C, or finding out why a struct is
larger than its fields. You need three questions answered, and DC answers
each with a compile-time operator.

## Size and alignment

- `sizeof(T)` is how many bytes a `T` occupies.
- `alignof(T)` is the multiple of which an address of a `T` must be.
- `offsetof(S, field)` is how many bytes from the start of `S` a field sits.

All three give a `usize` and are evaluated by the compiler.

```dc example=sizes
```

For the basic types the alignment equals the size: an `i32` lives at an
address divisible by 4, an `i64` at one divisible by 8. Hardware reads an
aligned value in one step, and some processors refuse a misaligned one
altogether, so the compiler never places a value carelessly. A pointer is
8 bytes on the 64-bit targets this chapter uses.

## Padding and field order

A struct stores its fields in the order you declare them. Each field starts at
the next address that is a multiple of its alignment, and the gap in between
is *padding*: bytes that belong to the struct but hold nothing.

```dc example=padding
```

`Loose` puts a single byte in front of an `i64`. The `i64` must start at a
multiple of 8, so offsets 1 to 7 are wasted; after `flag` another 7 bytes are
added at the end. `Tight` holds the same three fields, but the large one comes
first and the two small ones share the space behind it.

```text
Loose  | tag |.......| id (8 bytes) | flag |.......|   24 bytes
Tight  | id (8 bytes) | tag | flag |......|           16 bytes
```

The trailing padding exists because a struct's size is rounded up to its
alignment (the largest alignment of any field). Without it, the second
element of an array of `Tight` would start at offset 10 and its `i64` would be
misaligned.

The rule of thumb: declare fields from the largest alignment to the smallest.
You still choose field order for readability when memory does not matter;
you reorder when millions of values or a binary format do.

## Arrays and slices

An array `T[N]` is `N` elements one after the other with no padding between
them: element `i` is at `i * sizeof(T)`. That is possible because the size of
`T` already includes its trailing padding.

```dc example=stride
```

A slice `[]T` does not contain elements. It is a pointer to the first element
and a length, 16 bytes on a 64-bit target. A slice such as `row[1..3]` is
therefore free to create: it copies two words and points into the original
bytes. That is also why a slice can dangle: it cannot keep what it points at
alive.

## Bitfields

A field may be declared with a bit width: `u8 mode : 3;` takes 3 bits.
Neighbouring bitfields of the same type are packed into one storage unit;
a field of a different type starts a new unit, aligned for that type.

```dc example=bitfields
```

`mode` and `speed` fill one `u8` (3 + 5 bits). `level` is a `u16`, so it
starts a new unit at offset 2, and the struct is 4 bytes with alignment 2.
Use bitfields for binary formats or hardware registers; for everything else
a plain field is simpler.

## Common mistakes

- Assuming `sizeof(S)` is the sum of the field sizes. Padding makes it larger.
- Reordering fields of a struct that is shared with C code or a file format:
  the order is part of that layout.
- Treating a slice as the data. Copying a slice copies the pointer and the
  length, never the elements.
- Hard-coding numbers such as 8 or 16. Write `sizeof` and `alignof` so the
  code stays right on another target.
