# Functions that print

So far your functions computed a value and `main` printed it. Often the
function itself should print: a banner, a table row, a whole receipt. Such a
function has no value to return, but printing can fail (the output may be
closed or full), so it reports whether it worked. Its return type is
`Status(Error)`:

```dc
public Status(Error) print_banner([]const char text) {
    println("== {} ==", text)?;
    return Status::Ok;
}
```

## What `Status(Error)` and `?` mean

A `Status(Error)` is one of two things: `Status::Ok`, meaning "it worked", or
`Status::Err(...)`, meaning "it failed" and carrying the reason. `println`
itself returns a `Status`, and the `?` after it is a question: *did that
fail?*

- If `println` returned `Ok`, the `?` does nothing and the next line runs.
- If it returned an error, the `?` makes **your** function return that same
  error immediately. The rest of your function is skipped.

At the end, `return Status::Ok;` says that everything worked. The same `?`
works on your own functions. When `print_receipt` calls `print_item`, it
writes `?` after the call, and a failure travels upward, one function at a
time, until it reaches `main`:

```text
main --?--> print_receipt --?--> print_item --?--> println
  ^                                                    |
  +---------- the error travels back up ---------------+
```

When `main` itself returns an error, the program ends with exit code 1.
Leaving out the `?` does not cause a compiler error: the program compiles,
and a failure is silently ignored. Make `?` a habit after every call that
returns a `Status`.

A function can use `?` only if it returns a `Status` itself, so every
function that prints has the return type `Status(Error)`.

## Your task

`src/report.dc` has three functions that print nothing yet. Implement them:

| function                         | prints                                              |
| -------------------------------- | --------------------------------------------------- |
| `print_banner(text)`             | `== text ==`                                        |
| `print_item(name, price)`        | `name: price`                                       |
| `print_receipt(bread, milk)`     | the banner `Corner Shop`, items `bread` and `milk`, then `total` with their sum |

`print_receipt` must not call `println` itself: build it from the two
smaller functions, each followed by `?`. `main` already prints two receipts.
For `print_receipt(3, 2)` the output is:

```text
== Corner Shop ==
bread: 3
milk: 2
total: 5
```

## Acceptance criteria

- The project builds with `dpm build`.
- `main` prints both receipts exactly, separated by `---`.
- Every function returns `Status::Ok` after printing.
