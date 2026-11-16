# Copy a file without leaking handles

`data/poem.txt` ships with the exercise. The program copies it to
`poem-copy.txt`, counts the lines of the copy and then tries to copy a file
that does not exist.

The chunked read loop is the basic shape of file input:

```dc
u8[16] chunk = {};
while true {
    usize n = src.read(chunk[0..16])?;
    if n == 0 {
        break;
    }
    // use chunk[0..n]
}
```

`read` returns how many bytes it put in the buffer. It can return fewer than
the buffer holds, and `0` means the end of the file. The buffer is
deliberately tiny (16 bytes) so a 68 byte file needs several turns of the
loop: a program that reads only once passes with short files and fails on
real ones.

## Your task

`src/main.dc` is finished except for two functions:

1. `copy_file(from, to)` opens `from` for reading and creates `to`, copies
   every byte in 16-byte chunks and returns the number of bytes copied. Both
   handles must be closed on every path.
2. `count_lines(path)` returns the number of newline (`'\n'`) bytes in the
   file, reading in the same chunked way.

Open the source first. If it is missing, `?` returns before `create` ever
runs, so no empty destination file appears. The last line of the output
checks exactly that.

The program must print:

```text
copied 68 bytes
lines: 4
copy of missing file failed: not found
destination created: false
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- Every `open`/`create` is followed by a `defer ... close()`.
