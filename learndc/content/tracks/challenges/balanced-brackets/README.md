# Balanced brackets

A text is balanced when every `(`, `[` or `{` is closed by its own partner `)`, `]` or `}`, and
brackets are closed in the reverse order of opening. Other characters are ignored.

```text
"{[()]}"      balanced
"a(b)c[d]"    balanced
"(]"          not balanced (wrong partner)
"(("          not balanced (never closed)
")("          not balanced (closed before opened)
```

## Your task

Implement `public bool is_balanced([]const char text)` in `src/brackets.dc`.

Keep the open brackets on a stack: a `char[256]` array and a `usize depth`. An opening bracket
is pushed, a closing bracket must match the top of the stack and removes it. The text is
balanced when the stack is empty at the end. Nesting deeper than 256 counts as not balanced.

`src/main.dc` checks a few texts and must print:

```text
{[()]} balanced
(] not balanced
a(b)c[d] balanced
(( not balanced
```
