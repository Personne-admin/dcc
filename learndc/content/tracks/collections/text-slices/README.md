# Search and slice text

Text in DC is a slice of characters: `[]const char` is a pointer plus a
length, and `text.len` is the number of characters. `text[i]` reads one
character and `text[a..b]` is a new slice that borrows positions `a` up to
but not including `b`. No bytes are copied.

```dc
[]const char text = "hello world";
char first = text[0];            // 'h'
[]const char word = text[0..5];  // "hello"
```

A search that might find nothing returns `Optional(usize)`: write
`Optional::Some(position)` when found and `Optional::None` when not. A caller
tests it with `is_some()` and reads it with `unwrap_some()`.

## Your task

`src/words.dc` holds three stub functions, and `src/main.dc` already uses
them. Implement them with plain index loops:

1. `count_char(text, target)` returns how many characters of `text` equal
   `target`.
2. `find_char(text, target)` returns the position of the first match as
   `Optional::Some(position)`, or `Optional::None` if there is none.
3. `first_word(text)` returns the slice of `text` before the first space
   (`' '`), or all of `text` if it has no space. A text that starts with a
   space has an empty first word.

When all three work, the program prints:

```text
text: the quick brown fox
spaces: 3
first q at: 4
first word: the
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The program output matches the four lines above and the exit code is 0.
- The harness checks pass, including empty text and text without a match.
