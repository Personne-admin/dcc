# Four ways to say "that did not work"

Sooner or later a function cannot do what it was asked: the list is empty,
the text is not a number, the memory is gone. This lecture compares the four
ways DC lets you report that, and shows what `?` does for you. The lessons
after it practice each one.

## The sentinel and its trap

The oldest trick is a *sentinel*: a special value of the normal return type
that means "nothing". Here `-1` stands for "no ratings":

```dc example=sentinel
```

The program printed a bonus of 9 although nobody has rated anything. Nothing
in the type `i32` tells the caller that `-1` is special, and the compiler
cannot remind them. Every caller has to remember, and the one who forgets
gets a plausible, wrong number. Worse, sometimes `-1` is a real answer.

`Optional(i32)` fixes this by putting the "nothing" case into the type. The
caller cannot reach the number without a `match` that also handles
`Optional::None`.

## Choosing between the four

| Type | Holds | Use it when |
|---|---|---|
| `Optional(T)` | a `T` or nothing | there is no answer and the reason is obvious or does not matter |
| `Result(T, E)` | a `T` or an error `E` | there is an answer, or one of several reasons it failed |
| `Status(E)` | success or an error `E` | the work produces no value, only success or failure |
| a sentinel | a special `T` | almost never; only when a value like `-1` can never be real and everyone knows it |

Ask two questions. Does the function produce a value? If not, it is a
`Status`. Does the caller need to know *why* it failed? If yes, use an error
type (`Result` or `Status`); if "no answer" says it all, use `Optional`.

```dc example=three_kinds
```

`find_digit` returns `Optional` because "there is no digit" is the whole
story. `single_digit` returns `Result` because an empty text and a text that
starts with a letter are different problems, and the caller may react
differently. The outer `match` picks `Ok` or `Err`; a second `match` inside
the `Err` arm picks the reason.

## What ? does

Matching every call gets long when a function calls several fallible
helpers. The postfix `?` is a shorthand for one very common match:

- If the operation succeeded, continue. For a `Result`, `?` gives you the
  success value.
- If it failed, **return the error from the current function immediately**.

```text
i32 half = halve(n)?;

   halve(n) returns Result
        |
        +-- Ok(value) --> half = value, the next line runs
        |
        +-- Err(e) -----> return Err(e) from quarter, right here;
                          nothing after this line runs
```

A function that uses `?` on a `Result` or `Status` must itself return a
`Result` or `Status` with an error type that can take that error. That is
why the functions that print with `println(...)?` are declared
`Status(Error)`.

```dc example=early_return
```

For 300, `halve(300)?` fails on the first line of `quarter`. The second
`halve` never runs, and `report` receives the error and prints the failure
line. The caller still decides what to do about it.

## Common mistakes

- Using a sentinel such as `-1`, `0` or an empty text for "failed": the
  compiler cannot stop a caller from using it as a real value.
- Using `Optional` when the caller needs the reason: an empty text and a
  bad character both become `None` and cannot be told apart.
- Writing a call without `?` or `match` and ignoring the result: a
  discarded `Status` compiles silently, and the failure is lost. Always
  handle it.
- Expecting `?` to work in a function that returns `void` or a plain value:
  there is nowhere to return the error. Declare the function as `Status(...)`
  or `Result(...)`.
- Matching only the success case. A `match` must cover every variant, so
  the compiler reports the missing `Err` or `None`.
