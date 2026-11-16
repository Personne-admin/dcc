## Hint 1

`list.push_back_alloc(heap, song)?;` and `list.push_front_alloc(heap, song)?;` do the allocation and the linking in one call. `heap` is already a pointer, so pass it as it is.

## Hint 2

`play_next`: `Optional(SongNode*) node = list.pop_front();`. Return `Optional::None` when `node.is_none()`. Otherwise copy `node.unwrap_some().value` into a local `Song` before you free the node.

## Hint 3

`total_seconds`: `std::list::Iterator(Song) it = list.iterate();` then a `while` loop with `match it.next()`: `Optional::Some(node) => total += node.value.seconds,` and `Optional::None => more = false,`.
