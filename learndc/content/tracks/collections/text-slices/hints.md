## Hint 1

Inside `for usize i = 0; i < text.len; i++ { ... }` the expression `text[i]`
is the character at position `i`. Compare it with the target using `==`.

## Hint 2

`count_char` keeps a `usize` total and increments it on each match; it never
returns early. `has_char` and `first_word` can `return` from inside the loop
as soon as they find a match; the code after the loop handles "nothing
found" (`false` and the whole `text` respectively).

## Hint 3

In `first_word`, the line `return text[0..i];` inside the `if` returns the
characters before the space at position `i`. After the loop write
`return text;`. In `has_char`, the matching branch returns `true` and the
line after the loop returns `false`.
