# Cleanup on every failure path

Every `?` in a function is an exit. The function that allocates two arrays has
as many ways out as it has fallible calls, and each of them has to give back
what has been acquired so far, and nothing else. Code that only works when
nothing fails is the usual source of leaks and double frees: the failing path
runs in production once a month and in tests never. This lesson makes those
paths run, on purpose, in a test.

## Release where you acquire

`defer` registers a cleanup when the resource is acquired, and the cleanup
runs on every exit of the block, in reverse order. Compare these two ways of
writing the same function:

```dc example=leak
```

- `squares.deinit()` and `doubles.deinit()` are written at the end. The
  early exit through `?` skips them. When the second array fails to grow, the
  first one is leaked; when the first one fails at the very first allocation,
  nothing has been acquired yet and nothing leaks.
- The output is not a guess. The program counts allocations and frees in an
  allocator of its own, and says at which failure points memory is left.

## Fail on purpose: an injection sweep

The allocator in the example is a small `Probe`. It has the three methods of
an allocator policy (`allocate`, `resize_in_place`, `free`), borrows a
`Stats` record and keeps the number of calls. When the call count reaches
`fail_at`, `allocate` returns `AllocError::OutOfMemory`; every successful call
and every free are counted. `Array(T, Probe)` takes it as its allocator, and
`resize_in_place` returns `false` so that every growth allocates a new block
and frees the old one, which gives more failure points.

The technique:

1. Run the operation once with no failure to count its allocations
   (`clean.calls`).
2. For each `n` from 1 to that count, run it again with `fail_at = n`.
3. Each of those runs must report an error, and `allocs == frees` must hold.

```dc example=sweep
```

- With `defer` right after each acquisition, every one of the 8 failure points
  ends with all blocks returned: the sweep finds nothing.
- A failure in the first call needs no cleanup, in the fifth call it must
  release the two arrays and the block of the earlier growth; the sweep
  checks that each of them does.
- The test needs no knowledge of the implementation. If someone adds a
  third array, the number of failure points grows and the sweep covers it.

## Ownership that moves

Sometimes the function builds something to give to the caller. On success the
caller owns it; on failure the function must release it. The cleanup has to
know which case it is in:

```dc example=handover
```

- `handed_over` is set only after the last fallible step. The deferred block
  frees the array when it is still false.
- On success, `clean.allocs` is 4 and `clean.frees` is 3: three growth blocks
  were returned and one is alive, in the caller's hands. The counts only
  balance after `owned.deinit()`.
- The sweep for this function must treat success as a failure point too: a
  successful run is expected to leave one live block until the caller frees it.
  Here the sweep checks that every failing run leaves nothing behind.
- Cleanup code must not fail. Do not use `?` inside a `defer`; if a release can
  fail, decide there what that means (record it, ignore it, abort).

## Do the fallible work first

A function that changes state and can fail afterwards leaves a half-done
change. Order the steps so that every step that can fail comes before the
step that makes the change visible. `schedule` in the event scheduler of the
next chapter only increments its sequence number after the push succeeded;
`push` in the linked queue only links the node after the allocation worked.
When that order is impossible, undo explicitly in the error path and test
the path with the sweep.

## Common mistakes

- Writing cleanup at the end of the function instead of with `defer` right after
  the acquisition.
- Testing only the success path. The failing paths are exactly the ones
  that are never run by accident.
- Leaving an allocated resource owned by nobody after an early return: the
  hand-over flag was set too early (before the last `?`).
- Putting `?` in a `defer` statement. The compiler in this release does not
  support it, and a release that can fail needs a decision, not a propagation.
- Forgetting that the cleanup order is the reverse of the acquisition order.
