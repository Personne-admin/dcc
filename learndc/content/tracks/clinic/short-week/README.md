# The Sunday that never wins

## Case report

The shop owner says: "The weekly report is wrong. Sunday is always our best day, it is the day we
sell the most by far, but the report says Saturday is the busiest. The total looks right and
the quietest day looks right too."

Expected report for the sales of the week (120, 95, 140, 110, 180, 260, 310):

```text
total: 1215
busiest day: 7
quietest day: 2
```

Today the second line prints `busiest day: 6`.

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
