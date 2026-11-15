# Why the solution works

The program is a list of statements, and the output is those statements in
order. Stages 1 and 2 each add whole lines with `println` and placeholders:
the text stays in the format string, only the changing parts become
arguments.

The farewell shows why `print` exists. Three calls build one line: the first
two leave the cursor where it is, and the `println` at the end finishes the
line. The result is identical to a single `println("Have a good trip, {}!",
"Ada")`; building it in pieces just proves that you control exactly where
lines end.

The stages are checked with `contains` until the last one, so an unfinished
program can pass the early stages. The last stage compares the complete
output, so no extra or missing line can slip through.
