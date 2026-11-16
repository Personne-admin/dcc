# Threads, join and shared data

A **thread** is another path of execution inside your program, with its own
stack, running at the same time as the others (on another core, or taking
turns). Threads let a program use several cores. They also introduce a new
kind of bug, because every thread sees the same memory. This lecture shows
the safe way to use them. As in the previous lessons, the examples are
validated on Linux.

## Spawn and join

```dc example=sum_in_chunks
```

- `std::os::thread::spawn(function, arguments...)?` starts a thread that
  calls `function(arguments...)`. The function returns `void`; its parameters
  are whatever you pass. The result is a `Thread` handle.
- `thread.join()?` waits until the function has returned and reclaims the
  thread's stack. It consumes the handle: join each thread **exactly once**,
  a second call fails (the example prints `true` for the failure).
- Spawn allocates (the arguments are copied to a small block), so it can fail,
  hence `?`.
- A function that only needs numbers can take them as arguments. To give a
  thread something to write into, pass a **pointer**:
  `spawn(add_up, &chunks[i])`.

The pointer is why lifetimes matter: the chunk array lives in `main`, and
`main` joins every thread before it reads the chunks and before they go out
of scope. A thread must never outlive the memory it points at.

## One slot per thread

The cleanest way to share nothing is to share nothing: each thread gets its
own `Chunk`, computes into it, and writes only there. After the joins the
parent owns all the slots again and reads them in a fixed order (the order of
the array, not the order in which the threads finished). That makes the output
the same on every run, which is why this chapter always follows the pattern:

> threads compute, parent joins, parent prints.

Never print from several threads and expect an order; the scheduler decides.

## When memory really is shared

Sometimes threads have to update the same variable, such as a counter of how
many jobs finished. `value += 1` is three steps (read, add, write); two
threads can read the same old value and one increment is lost. That is a
**data race**, and its results change from run to run.

A `Mutex` lets one thread at a time into a region:

```dc example=locked_counter
```

- `lock()` waits until the mutex is free and takes it; `unlock()` releases
  it. A `Mutex` field starts unlocked when zero-initialised (`Counter counter = {};`).
- `defer counter.lock.unlock();` on the line after `lock()` makes every exit
  unlock. Forgetting an unlock on an early return hangs the next thread.
- Keep the locked region small. While one thread holds the lock, the others
  wait, so a lock around a long computation removes the parallelism.
- Lock the data, not the code: every access to `value` takes the same mutex,
  or the protection is gone.

`std::os::thread` also has `RwLock`, `Condvar`, `Semaphore`, `Once` and
thread-local keys for situations a plain Mutex does not fit; the examples in
this chapter do not need them.

## Limits to know

- `hardware_concurrency()` tells how many CPUs the program may use. It
  varies between machines, so never print it or let it change results.
- libdcext threads do not set up C library thread state. If you call C code
  that uses thread-local state (such as `errno`), call it from the main
  thread only.

## Common mistakes

- Not joining a thread, or joining it twice.
- Passing a pointer to a local variable that goes out of scope before the
  thread finishes.
- Updating shared variables without a lock, or locking only some of the
  accesses.
- Returning from a locked function without unlocking (use `defer`).
- Printing from threads and comparing the output with a fixed text.
