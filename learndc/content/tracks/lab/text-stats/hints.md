## Hint 1

Work one stage at a time. Every function starts from the same idea: walk the
text with an index or `for c in text` and keep a little state (inside a
word, the best so far, a counter) between characters.

## Hint 2

Draw the state by hand on a short text such as `"ab c"` before you code. For
`count_words` the state is one bool; for `longest_word` it is the start of
the current word and the best slice; for `most_common` it is a box number.

## Hint 3

The harness prints one line per case. Read the FAIL line, run the same
input in your head and compare. Edge cases are usually the empty text, a
word at the very end and ties.
