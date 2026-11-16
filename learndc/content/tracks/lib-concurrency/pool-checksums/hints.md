## Hint 1

Both functions are short. The task function is two lines: one cast, one assignment. Forgetting `wait_all` makes `main` read checksums that were not computed yet.

## Hint 2

`run_pool`: `ThreadPool* pool = std::os::thread_pool::create(workers)?;`, then `defer pool.destroy();`, then a `for` loop over the job indexes calling `pool.submit(...)?`, then `pool.wait_all();`.

## Hint 3

`pool.submit(checksum_task, &jobs[i] as void*)?;` and in the task `Job* job = context as Job*; job.checksum = std::hash::fnv1a_64(job.text as []const u8);`.
