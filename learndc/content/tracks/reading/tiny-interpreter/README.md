# Reading an interpreter loop

## What to look for

An interpreter is a loop: fetch the instruction at `pc`, advance `pc`, do what the instruction says. This one is a stack machine: instructions take their operands from a stack and push their results back.

The program is data, a list of `Op` values. Reading it means reading two things at once: the interpreter (how each operation behaves) and the program (what sequence it runs). Trace the stack as a column of numbers next to the instruction list.

## The program

```dc example=vm
```

## Reading it

The loop has the classic shape:

1. `Op op = program[m.pc];` fetches,
2. `m.pc += 1;` advances before executing, so a jump can overwrite `pc`,
3. `match op { ... }` executes.

`Op::Push(v)` and `Op::JumpNonZero(target)` carry a payload that the match arm names. `JumpNonZero` pops a value and, if it is not 0, replaces `pc` with `target`: that is how the countdown loops back to instruction 1.

Follow the stack through one round of the loop (`Dup`, `Print`, `Push(-1)`, `Add`, `Dup`, `JumpNonZero`): it starts with `[n]`, becomes `[n, n]`, `[n]`, `[n, -1]`, `[n - 1]`, `[n - 1, n - 1]` and then `[n - 1]` again. `Halt` reports the number of executed instructions and what is left on the stack.

## Common mistakes

- Forgetting that `pc` was already advanced when an instruction runs. A jump to `target` therefore runs the instruction at index `target` next.
- Mixing up the order of operands. `Add` pops `b` first, then `a`; for addition it does not matter, for subtraction it would.
- Ignoring the stack depth. A `Pop` with an empty stack would underflow; this machine has no check, so the program must be correct.
- Counting steps wrongly: every executed instruction counts once, including each repetition inside the loop and the final `Halt`.
