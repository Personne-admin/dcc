## Hint 1

Write the four operations as pointer changes, one at a time. `push` touches
`tail` (and `head` when the queue is empty), `pop` touches `head` (and `tail`
when the queue becomes empty). Both change `len`. Only after the pointers are
right, free the node.

## Hint 2

push: `Result(Node(T)*, AllocError) made = alloc.create_item();` then if
`made.is_err()` return `made.unwrap_err()`. Fill the node and set
`node.next = null`. If `queue.tail == null` the queue was empty and
`queue.head = node`, otherwise `queue.tail.next = node`. Then
`queue.tail = node`. pop: remember `node = queue.head` and its `value`, move
`queue.head = node.next`, set `queue.tail = null` when the head became null,
then free.

## Hint 3

```
Node(T)* node = queue.head;
T value = node.value;
queue.head = node.next;
if queue.head == null {
    queue.tail = null;
}
queue.len -= 1;
alloc.destroy_item(node);
return Optional::Some(value);
```
clear is `while queue.head != null { pop(queue, alloc); }`.
