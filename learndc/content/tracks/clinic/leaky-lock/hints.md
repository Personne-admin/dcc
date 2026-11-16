## Hint 1

Find which jobs leak by looking at which ones fail.

## Hint 2

A `?` can leave a function early, just like `return`.

## Hint 3

Add `defer release_lock();` straight after `take_lock();` and delete the release at the end of `run_job`.
