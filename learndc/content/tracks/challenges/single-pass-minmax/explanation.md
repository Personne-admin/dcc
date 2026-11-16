# Optimize: one pass for minimum and maximum

The two loops differed only in the comparison; merging them halves the reads without changing the answer. The key detail is to store the value of `probe::read` in a local variable: calling it once for the minimum test and once for the maximum test would count two reads again. The harness counts reads, not seconds, so the result is exact and the same on every machine.
