# Tracing and debugging loops

A loop that does the wrong thing is hard to see: the code looks right and the
answer is slightly off. This lecture gives you three tools: the trace table,
a list of the usual suspects and print debugging. Together they turn
"it does not work" into "this variable has the wrong value at this pass".

## Tracing by hand

A *trace table* has one column for each variable and one row for each pass
of the loop. You play the computer: write down what the variables are at the
start, then update them row by row, following the code exactly.

```dc example=trace_table
```

| pass | i | `i * i` | total after the pass |
| ---- | - | ------- | -------------------- |
| start |  |         | 0                    |
| 1    | 1 | 1       | 1                    |
| 2    | 2 | 4       | 5                    |
| 3    | 3 | 9       | 14                   |
| 4    | 4 | 16      | 30                   |

The table shows the answer (30) and where it comes from. When a program
disagrees with your table, the first row where they differ is the bug. Trace
with *small* inputs, such as `n = 0`, `1` and `2`: they are short and they
reveal the boundary problems.

## Off-by-one errors

The most common loop bug is being one pass too many or too few. It is
usually a mistake about the **ends**:

- `0..5` has 5 passes: 0, 1, 2, 3, 4. The end is excluded.
- `0..=5` has 6 passes: 0 to 5. The end is included.
- `i < n` and `i <= n` differ by exactly one pass.

Ask "how many passes should there be?" before you write the range. A fence
with 5 sections needs 5 rails but 6 posts, because both ends need a post:

```dc example=fence_posts
```

Typical signs of an off-by-one error: the answer is always a little too small
or too big, it is right for some inputs and wrong for others, and the
smallest inputs (`0`, `1`) behave strangely.

## Print debugging

When tracing by hand is too slow, let the program write the table. Print the
variables inside the loop, run the program, and compare what you see with
what you expect:

```dc example=print_debugging
```

Each line is one row of the trace table. Once you have found the bug, remove
the extra `println` lines again. If the output stops and never ends, the
loop's condition does not become false: look at the update line.

## Reading the check output

learndc runs your function with several inputs and reports the first
mismatches in a form like this:

```text
FAIL  sum_to(5): expected 15, got 10
```

The call tells you which input failed, `expected` is what the function should
return and `got` is what yours returned. Use it like this:

1. Take the smallest failing input and trace it by hand.
2. Look at the size of the difference. "Always one pass short" points at a
   range end. "Always off by the same number" points at a start value.
3. Look at which inputs *passed*. A bug that appears only for 0 or for
   negative numbers lives at a boundary.
4. If a check times out, a loop never ends. Check that the variable in the
   condition really changes in the body, and that no `continue` skips the
   update.

## Common mistakes

- Changing the code before understanding the failure. Trace first, then fix
  one thing and run the checks again.
- Tracing what you *meant* instead of what the code says. Follow the
  statements literally.
- Leaving debug `println` lines in the final program, which changes its
  output.
