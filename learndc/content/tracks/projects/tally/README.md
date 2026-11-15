# Tally a text

Small tools are often a helper module with a few public functions and a
`main` that prints a summary. This project does that for text. The functions
live in `src/words.dc`; `src/main.dc` calls them on a built-in text.

A word is a run of characters that are not separators. The separators are the
space, the tab (`'\t'`) and the newline (`'\n'`); `words.dc` already has
`is_space` for that. A loop over a `[]const char` visits one character at a
time:

```dc
for c in text {
    if c == '\n' {
        lines++;
    }
}
```

## Your task

Implement the three public functions in `src/words.dc`. They currently return
0.

- `count_words(text)`: how many words. Repeated, leading and trailing
  separators do not create words.
- `longest_word(text)`: the number of characters in the longest word, 0 for
  no words.
- `count_lines(text)`: the number of lines. Every `'\n'` ends a line; text
  that does not end in `'\n'` has a last, unterminated line. Empty text has 0
  lines and `"a\n"` has 1.

Then extend `main` so that it prints these three lines for its text:

```text
lines: 4
words: 10
longest word: 5
```

Do not change the text in `main`.

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints the three lines above and exits with code 0.
- The functions pass the edge cases checked by `learndc check`, including
  empty text, repeated spaces, tabs and final newlines.
