## Hint 1

`count_words` needs no library: one `for byte in text` loop and a `bool inside` that remembers whether the previous character was part of a word.

## Hint 2

`count_job` is one line. In `count_in_threads`, loop `for usize i = 0; i < jobs.len; i++` to spawn, and a second loop over `running.items()` to join, as in the lecture. Joining happens in a second loop so that all threads run at once.

## Hint 3

Spawn loop body: `running.push(std::os::thread::spawn(count_job, &jobs[i])?)?;`. Join loop body: `running.items()[i].join()?;`. Words: `if space { inside = false; } else if !inside { inside = true; words++; }`.
