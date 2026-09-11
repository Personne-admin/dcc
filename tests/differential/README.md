# Differential corpus

Executable differential programs for backend agreement checking. Each program
is compiled for every configured backend, run, and its exit status, stdout,
and stderr are compared byte-for-byte across configurations.

Run the whole corpus with make differential, or invoke the runner directly
with python3 tests/differential/diffrun.py tests/differential.

The runner accepts individual .dc files or directories, --configs as
backend:opt pairs, --compiler and --work overrides, and an optional
--baseline JSON file of expected artifact hashes. It always verifies that
the compiler and library archives did not change during the run.

Families:

- `basics/` - value, aggregate, map, slice, and match shapes.
- `error-panic/` - Result/Optional unwrap failures, ? early return,
  panics after partial side effects, and error paths inside match loops.
- `call-nesting/` - deep mixed int/float/struct call chains.
- `alloc-containers/` - standard-library map, sort, and array pressure.
- `physreg-shapes/` - call and return shuffles across fixed registers.
- `opt-pressure/` - inlining, loop-invariant, dead-code, CSE, and slice shapes.
- `check-failure/` - out-of-bounds and restricted-range failures.
