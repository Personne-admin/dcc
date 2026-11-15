# Why the solution works

```dc
public i32 apply(Op op, i32 acc, i32 operand) {
    return match op {
        Op::Add => acc + operand,
        Op::Sub => acc - operand,
        Op::Mul => acc * operand,
        Op::Div => if operand == 0 { acc } else { acc / operand },
    };
}

public i32 evaluate([]const Step steps) {
    i32 acc = 0;
    for step in steps {
        acc = apply(step.op, acc, step.operand);
    }
    return acc;
}
```

- `match` is exhaustive over `Op`, so adding a fifth operation later makes
  both `symbol` and `apply` fail to build until they are updated. There is
  no `_` arm to hide that.
- An `if ... else` with a value in each branch is an expression, so it can
  be the result of the `Div` arm. The zero check happens before the
  division, so the program never divides by zero.
- `evaluate` is a fold: a variable carries the result so far, and each
  iteration replaces it with `apply` of the next step. Starting from 0 makes
  an empty list evaluate to 0 with no special case.
- The steps are applied strictly in order and there is no operator
  precedence, so `Add 2, Mul 5, Add 1, Mul 2` is `((0 + 2) * 5 + 1) * 2`, which
  is 22.
- Splitting the work into `symbol`, `apply` and `evaluate` lets each function
  be tested alone, which is exactly what the checks do. `main` only prints
  and reuses the same functions.
