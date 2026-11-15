# Mini calculator

This challenge uses everything from the track. A calculator program is a list
of steps. Each step applies one operation to a running value that starts at 0:

```text
Add 5   ->  5
Mul 3   -> 15
Sub 4   -> 11
```

`src/calc.dc` defines the data:

```dc
public enum Op { Add, Sub, Mul, Div }

public struct Step {
    Op op;
    i32 operand;
}
```

A `Step` bundles an operation with its number. A list of steps is passed as a
`[]const Step`, a borrowed slice that you can walk with `for step in steps`
and read with `step.op` and `step.operand`.

## Your task

Implement the three placeholder functions in `src/calc.dc`:

- `symbol(op)` returns `"+"`, `"-"`, `"*"` or `"/"` for the operation.
- `apply(op, acc, operand)` returns the result of applying `op` to the running
  value `acc` and `operand`. Division is integer division. Dividing by zero
  must not crash: return `acc` unchanged instead.
- `evaluate(steps)` starts at 0, applies every step in order using `apply`,
  and returns the final value. An empty list evaluates to 0.

`src/main.dc` prints a trace of the first program and both results. Leave it
as it is. You choose how to write each match arm, but every `Op` variant must
be handled explicitly.

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints exactly:

```text
+ 5 -> 5
* 3 -> 15
- 4 -> 11
/ 0 -> 11
/ 2 -> 5
+ 20 -> 25
result: 25
second result: 3
```

- Both function checks pass, including negative numbers, `Div` by zero and
  an empty list of steps.
