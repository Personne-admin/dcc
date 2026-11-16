# The lock nobody released

## Case report

The operator reports: "After a night of batch jobs the scheduler is stuck. The report says two
locks are still held, although all four jobs are over. The jobs that failed are the ones that
look suspicious."

Expected output (the two refused jobs are normal, the locks are not):

```text
job 1: ok
job 2: failed
job 3: failed
job 4: ok
work done: 65
locks held: 0
```

Today the last line says `locks held: 2`.

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
