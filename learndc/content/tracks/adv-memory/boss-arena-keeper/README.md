# The Arena Keeper

An arena is a fixed buffer and a number: how much of the buffer is used. To
allocate, move the number forward. To free everything, set it back to zero.
Nothing is freed one by one, which is exactly why it is fast, and why
it fits a frame of a game: allocate everything the frame needs, then reset.

You write it in `src/arena.dc`. The struct and `new_arena` are given:

```dc
public struct Arena {
    []u8 memory;
    usize used;
    usize peak;
    usize allocations;
}
```

`memory` is the buffer, which the arena does not own. `used` counts every
byte taken, padding included. `peak` is the largest `used` ever reached and
`allocations` counts the successful allocations since the last reset.

Every stage is checked on its own. Each function in the starter returns a
placeholder.

## Stage 1: Read the legacy arena

Answer the three questions about the old code before you write your own.

## Stage 2: Alignment arithmetic

- `is_power_of_two(n)` is true for 1, 2, 4, 8 and so on, and false for 0.
- `align_up(value, align)` returns the smallest multiple of `align` that is
  at least `value`. `align` is a power of two, so rounding is a mask.

## Stage 3: Carve out aligned blocks

`alloc_bytes(arena, size, align)` returns `Result([]u8, ArenaError)`:

- `ArenaError::InvalidAlignment` when `align` is not a power of two.
- `ArenaError::OutOfSpace` when the block does not fit. Beware of `usize`
  overflow: a size close to the maximum must not wrap around.
- Otherwise a slice of exactly `size` bytes whose *address* is a multiple of
  `align`. The next free address is `memory.ptr as usize + used`. Align that
  address, subtract the base to get the start offset, and set
  `used = start + size`.

A refused request must change nothing: not `used`, not `peak`, not
`allocations`. A successful one raises `allocations` by 1 and `peak` when
`used` passes it.

## Stage 4: Typed allocation

Two generic helpers sit on top of `alloc_bytes`:

```dc
public Result(T*, ArenaError) alloc_one(T)(Arena* arena)
public Result([]T, ArenaError) alloc_many(T)(Arena* arena, usize count)
```

They ask for `sizeof(T)` bytes (times `count`) with alignment `alignof(T)`,
and turn the block into a `T*` or a `[]T`. `alloc_many` must refuse a count
whose byte size overflows `usize`. Callers name the type explicitly:
`alloc_one!Frame(&frames)`.

## Stage 5: Reset and statistics

- `reset(arena)` empties the arena (`used` and `allocations` back to 0) and
  keeps `peak`.
- `stats(arena)` returns a `Stats` with `used`, `remaining` (what is left of
  the buffer), `peak` and `allocations`.

## Stage 6: The frame loop

Complete `main` in `src/main.dc`. The `Frame` and `Particle` structs are
there. A local `u8[1024]` array has no guaranteed alignment, so first skip to
a 16-byte boundary with your own `align_up` and give the arena exactly 512
bytes of the aligned part. Then run frames 1 to 3:

1. `alloc_one!Frame`: set `number` to the frame number and `count` to
   `number * 10`.
2. `alloc_many!Particle` of `count` elements. Set each `life` to its index
   and add the lives up.
3. Print `frame N: C particles, life sum S, used U, free F` (`U` and `F` come
   from `stats`) and call `reset`.

After the loop print the peak and the bytes used, then try an allocation of
600 bytes and one with alignment 3 and print what each reports:

```text
frame 1: 10 particles, life sum 45, used 136, free 376
frame 2: 20 particles, life sum 190, used 256, free 256
frame 3: 30 particles, life sum 435, used 376, free 136
peak: 376 bytes
after reset: 0 bytes used
600 bytes: out of space
alignment 3: invalid
```

## Bonus: the allocator contract

At the end of `arena.dc` an `ArenaHandle` holds a pointer to an arena, and
`allocate`, `resize_in_place` and `free` are the three methods of the
allocator contract. `resize_in_place` and `free` are already written: an
arena never resizes or frees a single block. Write `allocate`: call
`alloc_bytes` and convert the error to an `AllocError`. Then an
`Array(i32, ArenaHandle)` can grow inside your arena, and a single `reset`
gives everything back.
