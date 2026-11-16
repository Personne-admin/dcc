# Project: Text Statistics

Needs: the collections chapter (arrays, slices and text), plus loops and
decisions.

A friend wants a small tool that reads a piece of text and reports how many
words it has, which word is longest, which letter is the favourite and how
often the common letters occur. The text is built into `src/main.dc`; the
measuring functions live in `src/textstats.dc` and each one is checked on its
own stage.

`is_letter` and `lower` are already written. In this project a *word* is a
maximal run of letters. Everything else (spaces, commas, digits, apostrophes)
separates words and is not part of one.

## Stage 1: Count the words

`count_words(text)` returns how many words the text has.

```text
"Hello, World!"      2 words
"  spaced    out "   2 words
"it's"               2 words   (the apostrophe separates)
```

## Stage 2: Find the longest word

`longest_word(text)` returns a slice of `text` (no copy) holding the longest word.
When several words are equally long the first one wins. A text without
words gives an empty slice, `text[0..0]`.

## Stage 3: Tally the letters

`count_letters(text, counts)` receives 26 counters, one per letter: box 0
counts `a` and `A`, box 1 counts `b` and `B`, up to box 25 for `z`. It adds to
the counters and ignores every other character. A character's code is a
number, so the box of a lowercase letter `c` is `c as u32 - 97`, because 97
is the code of `a`.

## Stage 4: Pick the favourite letter

`most_common(counts)` returns the letter whose box holds the largest count.
On a tie the earlier letter wins. When every box is 0 it returns `'-'`.
The box number turns back into a letter with `(box as u32 + 97) as char`.

## Stage 5: Print the report

Finish `main` so that it prints exactly this:

```text
characters: 77
words: 16
longest word: sleeps
most common letter: e (7)
letters seen 3 or more times:
e ####### 7
h #### 4
n ### 3
o ####### 7
r ### 3
s #### 4
t #### 4
u ### 3
```

A bar is one `#` per occurrence, printed with `print("#")?;` in a loop,
followed by the count. Every stage is checked on its own, so you can work in
order and see each one light up.
