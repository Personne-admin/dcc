# Why the solution works

```dc
public Status(Error) print_evens([]const i32 values) {
    Array(i32) evens = {};
    defer evens.deinit();
    collect_evens(&evens, values)?;
    for even in evens.items() {
        println("even: {}", even)?;
    }
    return Status::Ok;
}
```

- `Array(i32) evens = {};` starts an empty array that owns no memory yet. The
  first `push` allocates, and later pushes grow the storage as needed.
- `defer evens.deinit();` frees that storage when `print_evens` exits. It is
  placed immediately after the declaration, so every path out of the
  function releases it: the normal return, and the early return when `?`
  finds a failed `collect_evens` or `println`.
- `&evens` passes the array by address so that `collect_evens` pushes onto the
  caller's array, not onto a copy. Copying an owning array does not copy its
  elements and leads to two owners of one block of memory, so owning arrays
  are passed by pointer.
- `push` returns a status because allocation can fail. `?` hands such a
  failure to the caller, which is why both functions return `Status(Error)`.
- `items()` gives a slice of the elements for the `for` loop. It is only
  valid until the next `push`, which is fine here because all pushing is done
  before the loop starts.
