# A linked queue that owns its nodes

`src/queue.dc` declares a generic singly linked queue:

```dc
public struct Node(T) {
    T value;
    Node(T)* next;
}

public struct Queue(T) {
    Node(T)* head;
    Node(T)* tail;
    usize len;
}
```

Items enter at the `tail` and leave at the `head`, both in constant time. The
bodies of the four functions are placeholders:

- `push(queue, alloc, value)`: allocate one node with `alloc.create_item()`,
  fill it, link it after the tail and count it. If the allocation fails, return
  the error and leave the queue exactly as it was.
- `pop(queue, alloc)`: unlink the head, copy its value out, free the node with
  `alloc.destroy_item(node)` and return `Optional::Some(value)`; `None` for an
  empty queue.
- `peek(queue)`: the value at the head without removing it.
- `clear(queue, alloc)`: free every node, leaving an empty queue.

The allocator is a parameter of type `A*`, constrained by
`AllocatorPolicy(A)`. The queue never decides where memory comes from, and
the caller owns the queue and must free the nodes. `main.dc` uses a
fixed-buffer allocator and `defer queue::clear(&jobs, &alloc);`. The harness
uses allocators that count their calls and one that fails on demand. It
checks that

- items come out in the order they went in,
- `allocs == frees` after every pop sequence and after `clear`,
- a queue that was drained completely can be used again, and
- a failed `push` changes nothing.

`main.dc` is fixed and prints:

```text
print queue
start: 0 waiting, none
after five: 5 waiting, next 101
printed 101 and 102
after two prints: 4 waiting, next 103
 103 104 105 106
drained: 0 waiting, none
reused: 1 waiting, next 107
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The output above matches exactly and the exit code is 0.
- The harness checks pass: order, no leaks, reuse after draining and failure
  behaviour.
