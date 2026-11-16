## Hint 1

Start from the code on this page: every line of `capture` appears there. The new parts are building the command from `program` and `arguments`, and returning the `Captured` struct at the end.

## Hint 2

The order matters: create the pipe, point `stdout` at `pipe.writer.handle`, spawn, then close `pipe.writer`, then read, then wait. Put `defer command.deinit();` right after `Command::new(program)?` and the two `defer` closes right after the pipe is created.

## Hint 3

`for argument in arguments { command.arg(argument)?; }` adds the arguments. `count_lines` is a loop over the bytes counting `byte == '\n' as u8`. The result: `return Result::Ok(Captured { len = len, code = status.code });`.
