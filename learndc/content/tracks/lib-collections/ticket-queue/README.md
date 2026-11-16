# Run the ticket queue

A counter hands out numbered tickets. The queue is an `Array(u32)` that
`main` owns; every helper borrows it through a pointer. `main` is finished;
the six helpers in `src/main.dc` are stubs. Implement them with the array
methods from the lecture:

1. `open_queue(queue, expected)` reserves room for `expected` tickets and
   passes a failure on with `?`.
2. `take_ticket(queue, number)` adds a ticket at the end.
3. `vip(queue, number)` puts a ticket at the front, ahead of everyone
   (`insert`).
4. `cancel(queue, number)` removes that ticket wherever it is, keeps the
   others in order and returns `true`. An unknown ticket changes nothing and
   returns `false`. `std::slice::index_of(slice, value)` returns an
   `Optional(usize)` with the position of the first match.
5. `serve(queue)` removes and returns the ticket at the front, or `None` when
   nobody is waiting.
6. `spare(queue)` is how many more tickets fit without growing: capacity
   minus length.

Expected output:

```text
capacity 20, spare 20
queue: 101 102 103 104 105
queue: 99 101 102 103 104 105
cancel 103: true
cancel 500: false
queue: 99 101 102 104 105
serving 99
serving 101
serving 102
serving 104
serving 105
nobody is waiting
nobody is waiting
spare room: 20
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly and the exit code is 0.
- `cancel` keeps the remaining tickets in order (use `remove`, not
  `swap_remove`).
