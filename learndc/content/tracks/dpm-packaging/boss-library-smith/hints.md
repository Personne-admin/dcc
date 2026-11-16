## Hint 1

Stage 1 is questions only. For the code stages, start with the stage that is checked by `echo cells | dpm run smith` and work down the list.

## Hint 2

Implement one module at a time and run its command after each: cells, table and stats. The helpers can stay public until you reach the seal stage.

## Hint 3

Every stage's check is a command for smith. The seal, package, tests and release stages print what the audit saw, so read the lines that start with FAIL.
