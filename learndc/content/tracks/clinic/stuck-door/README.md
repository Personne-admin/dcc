# The door that locks while open

## Case report

The facility manager writes: "The door controller let the door be locked while it was open, so
the bolt hit the frame. The rule is simple: a door can only be locked while it is closed, and
only unlocked while it is locked. Pushing or pulling a locked door does nothing."

The test script sends: pull, lock, push, lock, pull, unlock, pull. The door should report the
state after each event:

```text
open
open
closed
locked
locked
closed
open
```

Today the second line already says `locked`.

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
