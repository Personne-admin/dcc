# Your first growable array

Time to own some heap memory. `src/main.dc` is an empty `main` with the
steps written down as comments.

## Your task

1. Create an empty array: `Array(i32) squares = {};`
2. On the next line, register its release: `defer squares.deinit();`
3. Push `n * n` for every `n` from 1 to 5. Write the loop as
   `for i32 n in 1..=5`: naming the type makes `n` an `i32` like the array's
   elements (a range loop counts in `usize` unless told otherwise). `push` can
   fail to allocate, so write `?` after it.
4. Print every element of `squares.items()` on its own line.
5. Add up the elements and print `5 squares, sum 55`, taking the 5 from
   `squares.len()`.

Expected output:

```text
1
4
9
16
25
5 squares, sum 55
```

The sum is an `i32` you add to inside the printing loop or in a loop of its
own. `len()` returns a `usize`, which prints like any other number.
