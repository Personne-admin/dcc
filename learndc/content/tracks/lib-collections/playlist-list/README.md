# A playlist as a linked list

`Array` keeps its elements side by side in one block. `std::list::List(T)`
is the other classic: every element lives in its own **node**, and each node
points at the one before and after it:

```text
head -> [Intro] <-> [Morning] <-> [Noon] <-> [Evening] <- tail
```

Choose a list when you add and remove at the ends (or in the middle, given a
node) often and never need `list[i]`: unlinking a node moves no other element
and no pointer to an element goes stale. Choose an `Array` for everything
else; it is faster to walk and simpler to own.

## Ownership

The list does not allocate by itself. A node is allocated from an allocator
you pass in, and whoever unlinks it must give it back to the same allocator.

```dc
import std::list;
import std::os::heap;

std::os::heap::Heap heap = {};
std::list::List(Song) list = std::list::new_list();
defer list.deinit(&heap);                 // frees every node still linked

list.push_back_alloc(&heap, song)?;       // new node at the back
list.push_front_alloc(&heap, song)?;      // new node at the front

Optional(std::list::Node(Song)*) node = list.pop_front();   // unlinks, does not free
// ... copy node.unwrap_some().value ...
std::list::destroy_node(&heap, node.unwrap_some());         // free it
```

- `push_back_alloc` and `push_front_alloc` can fail to allocate, so they return
  a status.
- `pop_front` and `pop_back` only unlink; the node still exists and is yours
  to `destroy_node`. Read what you need from it first.
- `deinit(&heap)` frees the nodes that are still linked. Register it with
  `defer` right after creating the list, as with an `Array`.
- Walk with an iterator: `list.iterate()` and `next()` give an
  `Optional(Node(T)*)`; the element is `node.value`. `reverse()` flips the list
  in place.

## Your task

`src/main.dc` stores `Song` values (title and length in seconds) in a
`Playlist`. `main` is finished. Implement the four stubs:

1. `add_last` appends a song at the back.
2. `add_next` puts a song at the front, so it plays next.
3. `play_next` removes the front song and returns it, or `None` when the list
   is empty. It must free the node it unlinked.
4. `total_seconds` adds up the `seconds` of every song using an iterator.

Expected output:

```text
queue: 4 songs, 650 seconds
  1. Intro (30s)
  2. Morning (180s)
  3. Noon (200s)
  4. Evening (240s)
reversed:
  1. Evening (240s)
  2. Noon (200s)
  3. Morning (180s)
  4. Intro (30s)
playing Evening
playing Noon
left: 2 songs, 210 seconds
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly; every node is freed (by `play_next` or by the
  `deinit` in `main`).
