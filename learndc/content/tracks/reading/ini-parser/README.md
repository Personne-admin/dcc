# Reading an INI parser

## What to look for

An INI file is plain text with `[sections]`, `key = value` lines and `;` comments. This program cuts such a text into pieces and prints `section.key = value` for every entry.

Before you read the code, decide what you expect to find:

- a helper that removes blanks around a piece of text,
- a function that splits one line at the `=`,
- a loop in `main` that finds the lines of the text and decides what each line is.

When you read unfamiliar code, start with the signatures and the data types, then the loop that drives everything, and only then the details.

## The program

```dc example=ini
```

## Reading it

Take the program apart by responsibility:

- `trim` returns a slice of the same text with blanks cut off on both sides. It does not copy anything: a slice is a window onto the original characters.
- `split_pair` looks for the first `=`. Found: it returns an `Optional` holding an `Entry` whose key and value are both trimmed. Not found: `Optional::None`.
- The loop in `main` walks over every index, including `text.len` so the last line is handled even without a final newline. When it reaches a `\n` (or the end) it cuts the line out with `text[start..i]`, trims it and moves `start` behind the newline.
- Then the cases are decided in order: blank or comment (`;`) is skipped, `[` starts a section, anything else must be a pair.

Notice the order of those tests. A comment such as `; a = b` contains an `=`, but it never reaches `split_pair` because it is skipped first.

## Common mistakes

- Reading the functions in the order they are written. The program starts in `main`; the helpers make sense once you know who calls them.
- Assuming a slice is a copy. `section` points into `text`; nothing is allocated.
- Forgetting the edge cases of a loop: the text may not end with a newline, which is why the loop runs up to `i <= text.len`.
- Believing an unusual input is rejected because it looks wrong. The parser rejects only lines without `=`; an empty key is accepted.
