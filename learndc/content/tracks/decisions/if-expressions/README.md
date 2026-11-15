# If as an expression

The last lesson used `if` as a *statement*: it chooses which lines run. Often
the only thing the branches do is pick a value. Instead of

```dc
i32 bigger = 0;
if a > b {
    bigger = a;
} else {
    bigger = b;
}
```

you can write the choice as an *expression*, which has a value like `a + b`:

```dc
i32 bigger = if a > b { a } else { b };
```

The value of each block is its last expression, written **without** a
semicolon. Both branches must produce a value of the same type, and the `else`
is part of the expression. An if expression can appear wherever a value can:
as an initializer, in `return`, as an argument of a call.

```dc
return if n < 0 { -n } else { n };
println("{}", if count == 1 { "item" } else { "items" })?;
```

A chain works too, and the first true condition wins:

```dc
i32 s = if n < 0 { -1 } else if n == 0 { 0 } else { 1 };
```

Use the statement form when a branch needs several lines or does something
(prints, changes a variable). Use the expression form when the question is
"which value?".

A semicolon after the value turns it into a statement and the block produces
nothing: `{ a; }` makes the compiler complain that the initializer has type
`void`.

## Your task

`src/pick.dc` has four functions that return placeholders. Implement each with
an if expression:

| function              | result                                              |
| --------------------- | --------------------------------------------------- |
| `max_of(a, b)`        | the larger of the two numbers                       |
| `abs_of(n)`           | `n` without its sign (`-n` when `n` is negative)    |
| `sign(n)`             | `-1`, `0` or `1` for a negative, zero or positive `n` |
| `item_word(count)`    | `"item"` when `count` is 1, otherwise `"items"`     |

`src/main.dc` prints some results; leave it as it is.

## Acceptance criteria

- The project builds with `dpm build`.
- The program output matches and both checks pass, including equal values,
  zero and negative numbers.
