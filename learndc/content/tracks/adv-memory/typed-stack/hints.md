## Hint 1

The stack never stores the allocator, it is passed to every call. So whoever
pushes a node and whoever frees it must use the same allocator. Think about
the order of operations: for `push`, allocate before you touch the stack; for
`pop`, take everything you need out of the node before you free it.

## Hint 2

`push`: declare `Result(Node*, AllocError) made = alloc.create_item();`,
return `made.unwrap_err()` when `made.is_err()`, otherwise unwrap the pointer
and set `value`, `next` and `top`. `pop`: handle `top == null` first, copy
the value, move `top` to `node.next`, free the node. `clear`: loop while `top`
is not `null`, calling `pop`.

## Hint 3

```
Node* node = made.unwrap();
node.value = value;
node.next = stack.top;
stack.top = node;
stack.len += 1;
```

In `pop`, `alloc.destroy_item(node);` comes after the lines that read `value`
and update `top` and `len`.
