# Owned arrays

A fixed array has its size in its type. `Array(T)` is the growable one: it
owns heap memory, `push` adds an element at the end, and the array must
give that memory back when you are done.

```dc
Array(i32) values = {};      // empty; {} is how you start one
defer values.deinit();       // runs when the function exits, however it exits

values.push(10)?;            // can fail to allocate, hence the ?
values.push(20)?;

for value in values.items() {   // items() is a borrowed slice
    println("{}", value)?;
}
```

- `defer` schedules the call for the end of the enclosing function, including
  an early return caused by `?`. Write it right after the declaration so the
  cleanup cannot be forgotten.
- `items()` lends you the elements as a slice. Pushing more can move the
  storage, so do not keep a slice across a `push`.
- To fill an array that belongs to the caller, take `Array(i32)*` and call
  `out.push(x)?` on it.

## Your task

Implement both functions in `src/numbers.dc`:

- `collect_evens(Array(i32)* out, []const i32 values)` pushes every even value
  of `values` onto `out`, in order. Test evenness with `value % 2 == 0`. A
  failed push must be returned with `?`.
- `print_evens(values)` creates its own `Array(i32)`, registers its cleanup
  with `defer`, calls `collect_evens` to fill it, and prints each element on
  its own line as `even: <n>`.

## Acceptance criteria

- The project builds with `dpm build`.
- Running it prints exactly:

```text
even: 8
even: 12
even: 20
```

- `collect_evens` appends to an array that already has elements.
