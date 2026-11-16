# Root cause: The lock nobody released

`run_job` has two exits: the `?` after `check` (failure) and the final `return Status::Ok;`. The manual `release_lock();` sits only before the second one, so each refused job leaks a lock. The numbers confirm it: two refused jobs, two locks held.

The method: make a table of exits. For each, ask which acquired resources it releases. Reading only the happy path hides the bug; the failing inputs expose it, so reproduce with an input that fails.

`defer` pairs the release with the acquisition in the source, so a new `return` or `?` added later cannot forget it.
