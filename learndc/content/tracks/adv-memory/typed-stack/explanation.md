# Why push allocates first

`push` does its only fallible step, the allocation, before it changes the
stack. If `create_item` fails, the function returns the error and `top` and
`len` still describe the old stack. Had it increased `len` first, a failed
push would leave a stack that claims one more value than it has.

`pop` follows the opposite rule for the same reason: it copies `value` and
unlinks the node, and only then frees it. After `destroy_item` the node's
memory is no longer yours, and reading `node.next` there would be a use after
free.

The stack does not remember its allocator. That keeps the struct small and lets
callers choose, but it makes the caller responsible for passing the same
allocator to `push`, `pop` and `clear`. The counting allocator in the harness
checks this contract: every successful allocation is matched by exactly one
free with the same size. In the 56-byte buffer the stack also reuses room after
a pop, because a `FixedBuffer` takes back the most recent block.
