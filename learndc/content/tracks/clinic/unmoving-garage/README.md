# The garage that never fills

## Case report

The parking attendant says: "Three cars drove into a garage with two free places. The board
should say two cars are parked and no place is free. It still says nobody is parked and both
places are free. Strangely, when a car leaves, the board does change."

Expected output:

```text
parked 2, free 0
parked 1, free 1
```

Today it prints `parked 0, free 2` and then `parked 0, free 2` again after the car left.

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
