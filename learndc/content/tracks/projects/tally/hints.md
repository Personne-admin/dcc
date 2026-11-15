## Hint 1

Walk through the text once with `for c in text`. Counting words is not the
same as counting spaces: what matters is the moment a word starts, a
non-separator that follows a separator (or the start of the text).

## Hint 2

Keep a `bool in_word` that is false at the start. For a separator set it to
false; for any other character, if it was false then a new word begins, so
count it, and set it to true. `longest_word` uses the same walk with a
`current` length that resets on separators and a `best` that keeps the
maximum. For lines, count the `'\n'` characters, then add one if the text is
non-empty and its last character is not `'\n'`.

## Hint 3

In `count_words`: `usize count = 0; bool in_word = false;` and in the loop
`if is_space(c) { in_word = false; } else { if !in_word { count++; }
in_word = true; }`. In `longest_word`: `current = 0` on a separator,
otherwise `current++` and update `best` when `current > best`. In
`count_lines`: remember `char last = '\n';` and assign `last = c;` in the
loop; after the loop `if last != '\n' { count++; }` (empty text keeps `last`
as `'\n'`, so it stays 0). In `main` add two more `println` calls like the
existing one.
