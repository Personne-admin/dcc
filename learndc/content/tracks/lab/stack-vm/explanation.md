# Why this machine works

The program lives in data. Turning text into a array of `Instr` values means that
running a program is only a loop over an array: fetch the instruction at `pc`,
`match` on it, change the stack, compute the next `pc`. A tagged enum is the
right type because each instruction carries exactly the data it needs
(`Push` a number, `Jump` an address, `Add` nothing) and `match` is exhaustive:
if you add a fourteenth instruction, the compiler points at `run` and
`listing`, the two places that must learn about it.

The assembler and the machine are separate on purpose. `parse_instruction`
decides what a line means and reports a precise error; `assemble` adds line
numbers; `run` never sees text. A fixed order of checks (unknown mnemonic,
operand count, operand value) makes every error unambiguous and testable.

Typed errors replace crashes. A stack machine fed random text will underflow,
divide by zero and jump into the void, and a real interpreter has to answer
each of these in a controlled way. `StepLimit` is the answer to a question no
analysis can answer in general: whether a program ever stops. Counting steps
turns "might loop forever" into an error you can print.

Memory follows the same rules as everywhere else: `run` owns its stack with
`defer`, `execute` owns the program and the output arrays, and every early
`return` or `?` releases them.
