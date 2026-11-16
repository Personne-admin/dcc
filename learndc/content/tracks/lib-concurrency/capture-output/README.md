# Capture a program's output

`Command` has no "run and give me the output" helper. You build it from two
pieces you already know: a child that writes to standard output, and a
**pipe**, a one-way channel in the operating system with a write end and a
read end. `std::os::pipe::create()?` returns both ends:

```dc
import std::os::pipe;

std::os::pipe::Pipe pipe = std::os::pipe::create()?;
defer pipe.reader.close();
defer pipe.writer.close();         // closing twice is harmless

command.stdout(pipe.writer.handle);    // the child's output goes into the pipe
std::process::Child child = command.spawn()?;
pipe.writer.close();                   // our copy of the write end: close it NOW
usize len = pipe.reader.read_all(buffer)?;   // read until the writer is gone
std::process::ExitStatus status = child.wait()?;
```

- `stdout(handle)` only *borrows* the handle: the child gets its own copy when
  it starts, and the pipe ends remain yours to close.
- **Close the write end before you read.** The reader sees "end of input" only
  when every copy of the write end is closed. The child's copy closes when it
  exits; yours closes when you say so. If you read first, `read_all` waits
  forever for data that can no longer come.
- `read_all(buffer)` fills the buffer or stops at the end of the input and
  returns the number of bytes read. A buffer that is smaller than the output
  keeps the first part and leaves the rest unread: if the child keeps writing
  into a full pipe it blocks, so only capture output of a size you know is
  bounded (or read in a loop).
- Waiting before reading can deadlock for the same reason; read first, wait
  second.
- The `defer`s make a failed `spawn?` (a program that does not exist) release
  the pipe too. They run in reverse order and the double close is safe.

## Your task

`src/main.dc` runs five programs through `run`, which is finished and prints a
summary of what `capture` returned. Implement:

1. `capture(program, arguments, buffer)`: build the `Command` (and `defer`
   its `deinit`), add every argument, connect a pipe to its standard output,
   spawn, close the write end, read everything into `buffer`, wait. Return the
   number of bytes and the exit code in a `Captured`. A program that cannot
   start makes `capture` return an error (the `?` does that).
2. `count_lines(output)`: the number of `\n` bytes in the captured output.

Expected output (the line `first line:` of the `silent` program ends with a
space):

```text
echo: 17 bytes, 1 lines, exit 0
  first line: alpha beta gamma
sort: 15 bytes, 3 lines, exit 0
  first line: apple
failing: 8 bytes, 1 lines, exit 4
  first line: partial
silent: 0 bytes, 0 lines, exit 0
  first line: 
missing: could not start
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly and no run hangs.
