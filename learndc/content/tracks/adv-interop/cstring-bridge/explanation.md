# What the two functions guarantee

`to_c` is where DC's world of lengths meets C's world of terminators. Its
checks are not decoration. `>=` reserves the byte for the terminator;
without it a full buffer would be written one byte past its end. The zero-byte
test keeps the meaning of the text intact: C only sees up to the first zero,
so a text containing one would silently be cut short. Returning a
`Result` makes both refusals visible to the caller instead of turning them into
a mysterious C-side truncation.

`from_c` is the opposite direction. The length is not stored anywhere in C,
so `strlen` has to find it, and the view then shares the C memory: no
copy. That is why the harness can check that the returned slice starts at
the same address as the input, and why the view must not outlive the memory
C owns. `null` becomes an empty slice, so callers do not have to repeat the
test after every `getenv`.

Keeping these two functions in one small module also keeps the risky
operations (raw pointers, `from_raw_const`, a terminator written by hand) in
one reviewed place, while `main` only deals with `Result` and slices.
