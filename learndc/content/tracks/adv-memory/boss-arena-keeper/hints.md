## Hint 1

Work in the order of the stages and run the checks after each one: every
stage has hints of its own. Read the legacy questions first; they describe
the two traps (rounding an already aligned value, and aligning an offset
instead of an address) that stages 2 and 3 test.

## Hint 2

`alloc_bytes` is the heart. Write its steps as a list before coding: check
the alignment, compute the aligned start offset from the address, check that
the block fits without overflow, update `used`, `peak` and `allocations`,
build the slice. `alloc_one` and `alloc_many` are short and only call it.

## Hint 3

```
usize base = arena.memory.ptr as usize;
usize start = align_up(base + arena.used, align) - base;
if start > arena.memory.len || size > arena.memory.len - start {
    return ArenaError::OutOfSpace;
}
```

After that: update the three counters and return
`Result::Ok(core::slice::from_raw(arena.memory.ptr + start, size))`.
