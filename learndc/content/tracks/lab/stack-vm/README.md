# Boss Fight: The Stack Machine

Needs: the data chapter (tagged enums and `match`), the errors chapter
(`Result`, `Status`, custom errors) and the memory chapter (`Array`, `defer`).

A *virtual machine* is a program that pretends to be a computer. Yours has a
stack of `i64` values and a program of thirteen instructions. A program starts
as text, one instruction per line, and an *assembler* turns the text into
values of a tagged enum:

```dc
public enum Instr {
    Push(i64), Add, Sub, Mul, Div, Dup, Over, Swap, Drop,
    Jump(usize), JumpIfZero(usize), Print, Halt,
}
```

| text         | does                                                          |
| ------------ | ------------------------------------------------------------- |
| `push N`     | push the number N                                             |
| `add` `sub` `mul` `div` | pop b, pop a, push `a + b`, `a - b`, `a * b`, `a / b` |
| `dup`        | push a copy of the top value                                  |
| `over`       | push a copy of the second value from the top                  |
| `swap`       | exchange the top two values                                   |
| `drop`       | pop and discard                                               |
| `jump T`     | continue at instruction number T                              |
| `jz T`       | pop a value; continue at T when it was 0                      |
| `print`      | pop a value and append it to the output                       |
| `halt`       | stop                                                          |

Instructions are numbered from 0. `;` starts a comment line and blank lines
are ignored. Running past the last instruction also stops the program. Here
is a program that counts down from 5:

```text
push 5     ; 0
dup        ; 1
print      ; 2
push 1     ; 3
sub        ; 4
dup        ; 5
jz 8       ; 6
jump 1     ; 7
halt       ; 8
```

`src/vm.dc` has the types, the error enum and four helpers (`print_error`,
`trim`, `split`, `parse_int`) already finished. You write the rest.

```dc
public enum VmError {
    @implicit_construction Alloc(AllocError),
    UnknownOp, MissingOperand, BadOperand, ExtraOperand, UnknownLabel,
    StackUnderflow, DivideByZero, BadJump, StepLimit,
}
```

## Stage 1: Run it in your head

Two questions: no code yet.

## Stage 2: Parse one instruction

`parse_instruction(line)` returns `Result(Instr, VmError)`. `push`, `jump` and
`jz` take exactly one operand, every other mnemonic none. Jump targets are
numbers 0 or more. Errors, in this order:

1. `UnknownOp`: empty line or unknown (lower case) mnemonic,
2. `ExtraOperand`: too many words (checked before the operand value),
3. `MissingOperand`: `push`, `jump` or `jz` without operand,
4. `BadOperand`: the operand is not a number, or a negative jump target.

## Stage 3: Assemble a program

`assemble(lines, program, bad_line)` appends one `Instr` per instruction line
and stops at the first error, storing the 1-based line number in `*bad_line`.
Lines that are empty after trimming, or start with `;`, are skipped but still
count in the line numbers.

## Stage 4: Run the machine

`run(program, out, max_steps)` executes from instruction 0. `out` receives the
printed values. Binary operations take `b` from the top and `a` below it.
Refusals, which stop the run (what was printed stays in `out`):

- `StackUnderflow` when an instruction needs more values than the stack holds,
- `DivideByZero` for `div` with `b == 0`,
- `BadJump` when a jump target is greater than `program.len` (equal to it
  means "end of the program"),
- `StepLimit` when `max_steps` instructions have already run and another is due.

## Stage 5: Print a listing

`listing(program)` prints one line per instruction: the address in 3
columns, two spaces, the mnemonic and the operand.

```text
  0  push 5
  1  dup
  9  jz 12
```

## Stage 6: The machine room

`execute(name, lines)` in `src/main.dc` prints `== name ==`, assembles the
lines, prints the listing, runs with a limit of 1000 steps and prints
`output:` followed by the printed values, each after a space, then the runtime
error if any:

```text
== countdown ==
  0  push 5
  ...
output: 5 4 3 2 1
== spin ==
  0  push 1
  1  jump 0
output:
error: step limit reached
== typo ==
error: line 3: unknown instruction
```

An assembly error prints only the error line (no listing, no output line).

## Bonus: labels

`assemble_labels` accepts lines such as `again:` that name the address of the
next instruction, and lets `jump` and `jz` use a name instead of a number.
Unknown names give `UnknownLabel`; bad lines report their line number exactly
like `assemble`.
