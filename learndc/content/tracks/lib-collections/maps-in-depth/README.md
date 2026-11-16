# Maps beyond lookup

The memory chapter used a `Map` to store and find values. A map has more
tools than `put` and `get`, and one important rule about iteration.

## The calls

```dc example=map_calls
```

- `put(key, value)` inserts or replaces. It returns a
  `Result(Optional(V), AllocError)`: the `Result` says whether memory was
  available, the inner `Optional` carries the value that was replaced (or
  `None` for a new key). Writing `visits.put(7, 1)?;` ignores the old value
  and still passes an allocation failure on.
- `get(key)` returns a copy as `Optional(V)`; `.some_or(0)` supplies a default
  for a missing key.
- `get_or_put(key, fallback)?` returns a **pointer** to the value, inserting
  `fallback` first when the key is new. That is the shortest way to count:
  `*map.get_or_put(key, 0)? += 1;`.
- `remove(key)` takes the entry out and returns its value as an `Optional`.
- `contains`, `len`, `is_empty` and `clear` do what their names say. `clear`
  keeps the memory, like `Array`.

A pointer from `get_or_put` or `get_ptr` points into the table. The next
insertion that makes the table grow moves every entry, so use the pointer at
once and look the key up again later.

## Walking every entry

`map.iterate()` gives an iterator; `next()` returns an `Optional(Entry(K, V))`
until the table is exhausted. An `Entry` holds two pointers, `key` and
`value`, so read them with `*entry.key` and `*entry.value`.

The iterator type has the allocator as a third argument, which is why the
example gives it a name with `using`.

## Order is not part of the deal

A `Map` is a hash table: each key is hashed and stored where the hash says.
The iteration order depends on the hashes and on how big the table has
grown. It is not insertion order and not sorted order, and it may differ
between library versions. A program whose printed output must be the same
every time therefore never prints in iteration order. It collects the keys
and sorts them:

```dc example=sorted_dump
```

Collect with `next()` into an `Array(u32)`, sort with `std::sort::sort`, then
look every key up. This costs one extra array but makes the output
reproducible and testable.

## Which keys work

A key type needs two things: a hash (`std::hash::hash_of`) and an equality
test. The library provides them for integers, `char`, `bool` and pointers.
At the time of writing, text (`[]const char`) and struct keys do not compile
as `Map` keys: the library's equality helper is ambiguous for text, and a
struct cannot supply its own equality. Use an integer id as the key (a line
number, a code, a bucket number), or keep text in an `Array` of structs and
search it with `std::slice::eq`, as the Inventory Keeper did.

## Common mistakes

- Printing straight from the iterator and expecting a particular order.
- Inserting into the map while iterating it: the table may move under the
  iterator.
- Keeping the pointer from `get_or_put` across another `put`.
- Forgetting `defer map.deinit();`: the table is heap memory.
- Using `get` and writing to the copy: it changes nothing. Use `get_ptr` or
  `get_or_put` to modify a value in place.
