# Reading a ring buffer

## What to look for

A ring buffer is a queue with a fixed capacity: four slots, a `head` that says where the oldest element is, and a `count`. Nothing is moved when an element leaves; `head` just advances, and positions past the end of the array wrap around to the start.

While you read, keep a small table on paper: `head`, `count` and the four slots after every call. Tracing state by hand is the main skill of this lesson.

## The program

```dc example=ring
```

## Reading it

The key line is in `push`:

    usize tail = (ring.head + ring.count) % capacity(ring);

The next free slot is `count` places behind `head`, and `% 4` wraps it into the array. `pop` reads `items[head]`, then advances `head` the same way and lowers `count`.

Two guards protect the state: `push` refuses when `is_full`, and `pop` returns `Optional::None` when `count` is 0. Both return a value that the caller can inspect, so `main` shows how a refused `push` is reported while the result of the later `push(&ring, 60)` is ignored.

The array is never cleared. Popped values stay in their slots until a later push overwrites them; the buffer is correct because only `head` and `count` say what is valid.

## Common mistakes

- Reading `head` as "the newest element". It is the oldest; new elements go behind the others.
- Forgetting that `count` is not an index: `head + count` is the tail, and only after the wrap.
- Thinking `pop` erases the slot. The old value remains until it is overwritten.
- Counting five pushes in a buffer of four: the fifth is dropped with a message.
