## Hint 1

A `for` loop can visit every position: `for usize i = 0; i < text.len; i++`.
Inside it, `text[i] == target` compares one character with the target.

## Hint 2

`count_char` keeps a `usize` total and increments it on each match.
`find_char` and `first_word` can return from inside the loop as soon as they
find a match; the code after the loop handles "nothing found" (`Optional::None`
and the whole `text` respectively).

## Hint 3

In `first_word`, the line `return text[0..i];` inside the `if` returns the
characters before the space at position `i`. After the loop write
`return text;`. In `find_char`, the matching branch returns
`Optional::Some(i)`, and the line after the loop returns `Optional::None`.
