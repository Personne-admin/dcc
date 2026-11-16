# A stack that owns its nodes

A stack of `i32` values built as a linked list. Every value lives in a
`Node`, and every `Node` is a separate block from an allocator that the
*caller* chooses, so the same code can run on the heap, in a fixed buffer or
in an arena.

```dc
public struct Node {
    i32 value;
    Node* next;
}

public struct Stack {
    Node* top;
    usize len;
}
```

`src/stack.dc` declares three generic functions. Each takes the allocator as
a pointer `A* alloc`, with `A` constrained by `AllocatorPolicy(A)`:

- `push(stack, alloc, value)` allocates a node with `alloc.create_item()`,
  fills it, links it in front of `top` and increases `len`. It returns
  `Status(AllocError)`: when the allocation fails the stack must stay exactly
  as it was.
- `pop(stack, alloc)` removes the top node, gives it back with
  `alloc.destroy_item(node)` and returns `Optional::Some(value)`, or
  `Optional::None` for an empty stack. Read `value` and `next` *before* you
  free the node.
- `clear(stack, alloc)` pops everything, so no node is left.

The starter compiles and does nothing. `src/main.dc` is finished; it pushes
four values, pops two, clears the stack and prints:

```text
pushed 4 values
popped 4
popped 3
2 values left
cleared: 0 values left
```

A second check runs your stack on an allocator that counts: every node that
was allocated must be freed, once, with the size it was allocated with. It
also runs the stack in a 56-byte `FixedBuffer`, where only three nodes fit,
to see that a failed `push` changes nothing.

Reminder: the typed helpers infer the element type from where the result is
stored, so write `Result(Node*, AllocError) made = alloc.create_item();`.
