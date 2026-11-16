## Hint 1

Run the starter and read the numbers. Its size is larger than the sum of its
fields: the difference is padding that the compiler inserts so every field
starts at a multiple of its own alignment.

## Hint 2

`field_bytes` is one `return` of five `sizeof` terms added together, one per
field type: u8, u64, u8, u32 and u16. For the layout, move the `u64` field to
the top and keep going down: u32, u16, then the two u8 fields.

## Hint 3

The struct becomes `u64 id; u32 length; u16 port; u8 kind; u8 flags;` and
`field_bytes` returns `sizeof(u8) + sizeof(u64) + sizeof(u8) + sizeof(u32) +
sizeof(u16)`.
