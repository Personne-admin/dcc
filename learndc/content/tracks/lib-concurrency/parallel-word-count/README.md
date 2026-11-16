# Count words on several threads

Five texts, five jobs. A `Job` holds a text and a slot for its word count.
`main` is finished: it runs the jobs, then prints the counts in job order.

You implement three functions, from the inside out:

1. `count_words(text)`: a word is a run of characters that are not space,
   newline or tab. Track whether you are `inside` a word: the count goes up
   when you move from outside a word to a non-space character. Empty text has
   0 words; repeated separators do not make empty words.
2. `count_job(job)`: the thread's function. It reads `job.text` and writes
   `job.words`, and touches nothing else.
3. `count_in_threads(jobs)`: spawn one thread per job and pass it a pointer to
   its own job, `&jobs[i]`. Keep the `Thread` handles in an `Array`
   (`std::os::thread::Thread` is the type), then join every one of them.
   A thread handle is not copied around: push it into the array right after
   spawning.

```dc
running.push(std::os::thread::spawn(count_job, &jobs[i])?)?;
```

Two separate `?`: the inner one is spawn failing, the outer one is the array
failing to grow.

Expected output:

```text
text 1: 4 words
text 2: 5 words
text 3: 0 words
text 4: 1 words
text 5: 5 words
total: 15 words
```

## Acceptance criteria

- The project builds with `dpm build`.
- The output matches exactly, in text order, however the threads are
  scheduled.
- All threads are joined before the results are read.
