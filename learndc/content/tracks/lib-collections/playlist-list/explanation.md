# Who frees a node?

Every node is allocated by `push_back_alloc` or `push_front_alloc` from the
`heap` that `main` owns. A node leaves the list in one of two ways. If it is
still linked when the program ends, `defer list.deinit(&heap)` frees it. If
`play_next` unlinks it, `play_next` is now the node's owner: it copies the
song out first (the node is about to disappear) and then calls
`destroy_node`. Returning `n.value` after the free would read released
memory, which is why the copy comes first.

`total_seconds` takes a `const Playlist*`: iterating does not change the
list, and the iterator is a small value on the stack. Nothing is allocated
during the walk, so it cannot fail and returns a plain number.

The list gives O(1) insertion at both ends without moving any other song. An
`Array(Song)` would have to shift every element to put a song at the front;
the price of the list is one allocation per song and no indexing.
