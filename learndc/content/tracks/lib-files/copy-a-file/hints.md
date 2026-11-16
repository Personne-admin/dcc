## Hint 1

Both functions are the same loop: read a chunk, stop when `read` returns 0,
otherwise do something with the first `n` bytes. Open the file first and put
`defer` on the next line.

## Hint 2

In `copy_file`: open `from`, `defer src.close();`, create `to`,
`defer dst.close();`, then the loop. Inside the loop write only the bytes that
were read, `dst.write_all(chunk[0..n])?`, and add `n as u64` to a total.
In `count_lines`, loop over `chunk[0..n]` and count the bytes equal to
`'\n' as u8`.

## Hint 3

```dc
File src = std::os::file::open(from, std::os::file::read_only())?;
defer src.close();
File dst = std::os::file::create(to)?;
defer dst.close();
```

Declare `u64 total = 0;` before the loop, and finish with
`return Result::Ok(total);`.
