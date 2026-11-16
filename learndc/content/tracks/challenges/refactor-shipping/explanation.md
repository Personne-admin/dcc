# Refactor: integer codes to an enum

The behaviour did not change, the possible states did: an `i32` can be 7, a `Speed` cannot. A `match` without a wildcard arm makes the compiler list every function that must change when a speed is added, which the old `if` chains could not. The harness tests the module on numbers the program never uses; the run check guarantees the refactoring kept the output.
