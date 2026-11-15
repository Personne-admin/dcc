# Reading compiler errors

When the compiler cannot build your program it prints a diagnostic. Every
one follows the same shape. Here is an error from a different program, one
that misspelled a variable name:

```text
error: unknown name `totl`
  --> src/main.dc:5:26
    |
  4 |     i32 total = price * 3;
  5 |     println("Total: {}", totl)?;
    |                          ^^^^
  6 |     return Status::Ok;
    |
```

Read it from top to bottom:

1. `error: ...` is what went wrong, in one sentence. Names and types in
   backticks are exactly what the compiler saw.
2. `--> file:line:column` is where. The path may be long; look at the end:
   file `src/main.dc`, line 5, column 26. Start there.
3. The code frame repeats the lines around the problem, numbered, with
   `^^^^` under the part the compiler is complaining about.
4. Extra lines marked with `~~~` are notes that point at related code, such
   as the declaration the compiler compared your code with. They often hold
   the answer.

Fix the first error first. Later errors are sometimes only consequences of
it, and one fix may clear several. Run `dpm build` again after each fix.

## Your task

`src/main.dc` should print a receipt for three notebooks that cost 250 cents
each:

```text
Item: notebook
Total: 750 cents
```

It does not build. Check it, read the diagnostic with the steps above, and
repair the line it points at. Do not change `total_price`; the mistake is in
how `main` uses it.

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints exactly the two lines above and exits with code 0.
