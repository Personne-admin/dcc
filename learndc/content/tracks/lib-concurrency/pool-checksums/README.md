# Checksums on a thread pool

Starting a thread per job is fine for five jobs. A program with ten thousand
jobs wants a fixed number of **worker threads** that take jobs from a queue
as they become free. That is `std::os::thread_pool`:

```dc
import std::os::thread_pool;

std::os::thread_pool::ThreadPool* pool = std::os::thread_pool::create(workers)?;
defer pool.destroy();                         // shuts the pool down and frees it

pool.submit(task_function, context)?;         // queue one task
pool.wait_all();                              // block until the queue is empty and every worker is idle
```

- `create(n)?` starts `n` worker threads (`n` must be at least 1) and returns
  a pointer to the pool. The pool is heap memory: `destroy()` joins the workers
  and releases it. Register it with `defer` right away.
- A task is a function `void f(void* context)` plus the `context` pointer it
  receives. `submit` queues it and returns at once; any idle worker may run it.
  Tasks start in the order submitted, but they run at the same time and finish
  in any order.
- The context is a plain `void*`: cast a typed pointer with `&jobs[i] as void*`
  when submitting, and back with `context as Job*` inside the task. The memory
  must stay valid until the task has run: that is what `wait_all` guarantees
  before `jobs` goes out of scope.
- `submit` can fail (`?`): the pool queue grows on demand.
- Tasks must not print or share unprotected state. Like the threads before,
  each task writes only to its own job.

The pool size is a tuning knob (usually the number of cores). A correct
program gives the same answers for 1 worker and for 16, because each job is
independent and the results are read in job order afterwards.

## Your task

`src/main.dc` has a `Job` (name, text and a slot for the checksum) and six
jobs in `main`. Fill in:

1. `checksum_task(context)`: cast `context` back to `Job*` and store
   `std::hash::fnv1a_64(job.text as []const u8)` in `job.checksum`.
2. `run_pool(jobs, workers)`: create the pool, `defer` its `destroy`, submit one
   task for every job (the context is the address of that job), then wait for
   all of them.

`main` runs the six jobs on 3 workers, prints the checksums in job order,
clears them, runs everything again on a single worker and checks that the
sums are identical.

Expected output (checksums are 16 hex digits):

```text
empty    cbf29ce484222325
letter   af63dc4c8601ec8c
greeting 779a65e7023cd2e7
numbers  50c0aafd8b4330b2
sentence 7404cea13ff89bb0
repeat   d63946adcbe9c9c5
same sums with one worker: true
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly.
- The pool is destroyed on every path out of `run_pool`.
