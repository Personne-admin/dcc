# Why the order matters

In the original order, `kind` is followed by an `i64`-sized field that must
start at a multiple of 8, so seven bytes of padding appear after `kind`.
`flags` and `length` pull in more gaps, and the whole struct is rounded up
to 32 bytes to keep the next element of an array aligned. The fields only
need 16.

Declared from the largest alignment to the smallest, each field lands
exactly where the previous one ended: 0, 8, 12, 14 and 15, and the size 16 is
already a multiple of the alignment 8, so no trailing padding is needed.

`field_bytes` uses `sizeof` on each field type instead of the number 16, so
the report stays correct if a field type changes. A padding of 0 is the
proof that the layout is tight.
