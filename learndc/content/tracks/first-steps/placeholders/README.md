# Placeholders, text and numbers

You have used one `{}` so far. A format string can hold as many as you like.
The arguments after the string fill them from left to right, one for each:

```dc
println("{} and {} make {}", "salt", "pepper", 2)?;
```

prints `salt and pepper make 2`. The first `{}` gets `"salt"`, the second
gets `"pepper"`, the third gets `2`. Text goes in double quotes; numbers are
written bare. The order of the arguments is the order of the holes, so
`println("{}: {}", 7, "Ada")?;` prints `7: Ada`, which is probably not what
you wanted.

## print and println

`println` prints its text and then starts a new line. `print` prints the text
and stays on the same line, so the next output continues right after it:

```dc
print("Loading")?;
print("...")?;
println(" done")?;
```

prints `Loading... done` on one line. Both accept placeholders the same way.
If you ever need a literal brace in the output, write it doubled: `{{` prints
`{`.

## Your task

`src/main.dc` prints a heading and has four comments that mark where your
lines go. Fill them in so the program prints:

```text
Cafe order
Guest: Ada, table 7
Items: 2 x tea, 1 x cake
Total: 9 coins
Enjoy your meal, Ada!
```

- The guest line is one `println` with two placeholders.
- Write the items line in two calls: `print("Items: ")?;` first, then a
  `println` with two placeholders for the rest of the line.
- Every line ends with a newline except the part you print with `print`.

## Acceptance criteria

- The project builds.
- The program prints exactly the five lines above and exits with code 0.
