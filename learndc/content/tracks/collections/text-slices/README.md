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

A character literal such as `'l'` uses single quotes and compares with `==`.

## The index loop

`for c in text` visits the characters, but sometimes you need to know the
position as well. For that, C-style `for` has three parts:

```dc
for usize i = 0; i < text.len; i++ {
    // body
}
```

1. `usize i = 0` runs once, before the loop: it declares the counter.
2. `i < text.len` is checked before every round. When it is false the loop
   ends.
3. `i++` runs after every round and adds 1 to `i`.

It is the `while` loop you already know with the counter bookkeeping moved
to the top:

```dc
usize i = 0;
while i < text.len {
    // body
    i++;
}
```

Tracing `"abc"` shows which values `i` takes. The condition fails at 3, so
`text[3]` is never read:

```text
i = 0  0 < 3  body sees 'a'
i = 1  1 < 3  body sees 'b'
i = 2  2 < 3  body sees 'c'
i = 3  3 < 3  false: the loop ends
```

Using `i <= text.len` instead would let `i` become 3 and read past the end,
which stops the program with an index panic. An empty text has `len` 0, so
the body never runs.

## Your task

`src/words.dc` holds three stub functions, and `src/main.dc` already uses
them. Implement them with the index loop:

1. `count_char(text, target)` returns how many characters of `text` equal
   `target`. It must look at every position.
2. `has_char(text, target)` returns `true` when the character occurs at least
   once. Return from inside the loop as soon as you find it; only after the
   loop do you know the answer is `false`.
3. `first_word(text)` returns the slice of `text` before the first space
   (`' '`), or all of `text` if it has no space. A text that starts with a
   space has an empty first word.

When all three work, the program prints:

```text
text: the quick brown fox
spaces: 3
has q: true
has z: false
first word: the
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The program output matches the five lines above and the exit code is 0.
- The harness checks pass, including empty text and text without a match.
