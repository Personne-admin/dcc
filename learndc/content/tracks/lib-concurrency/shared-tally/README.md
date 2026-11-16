# A tally shared by many threads

Five workers each count the words of a text and add them to **one** shared
`Tally` (each worker repeats its text 20000 times, so the threads really
collide). The worker, the spawning code and `count_words` are
finished. The tally has a `Mutex`:

```dc
struct Tally {
    std::os::thread::Mutex lock;
    u32 words;
    u32 documents;
    u32 limit;
}
```

Implement the three functions that touch `words`:

1. `add_word(tally)` adds 1 to `tally.words`. Take the lock, register
   `defer tally.lock.unlock();` on the next line, then add.
2. `try_add(tally, amount)` adds `amount` only when the result does not exceed
   `tally.limit`; otherwise it returns `false` and changes nothing. The check
   and the update must happen under the **same** lock acquisition (another
   thread could slip in between a check outside the lock and an update
   inside it). Because the refusal is an early `return`, `defer` is how you
   avoid a thread that never unlocks.
3. `words_so_far(tally)` returns the current `tally.words`, read under the lock.
   Reading while another thread writes is also a data race, even though it
   only looks at the value.

`main` runs the workers, then calls `try_add` three times on the single main
thread. If `try_add` forgets to unlock when it refuses, the next call
waits for ever; the check then ends after its time limit.

Expected output:

```text
after the workers: 300000 words, 5 documents
add 2: true
add 2 again: false
add 1: true
final: 300003 words
```

## Acceptance criteria

- The project builds with `dpm build`.
- The count after the workers is exactly 300000: without the lock, threads
  overwrite each other's increments and the number is lower (and different on
  every run).
- Every `lock()` has a matching `defer unlock()`.
