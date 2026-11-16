# Compile-time programs

The core course showed that `using usize N = factorial(10);` runs a function
while compiling. The same mechanism builds whole tables and lets a
configuration constant decide what code exists. This lesson is about using it
on purpose: what it can compute, what it cannot, and how a generic API turns
a bad request into an error that names the problem.

## What the compiler can run

A value alias may take the result of a call, and the result may be a whole
array. The compiler runs the function (*compile-time function evaluation*,
CTFE), stores the array in the program and the program only reads it:

```dc example=sieve
```

- `build_primes` is an ordinary function with loops, an array local and
  index assignment. It can still be called at run time; here it only runs in
  the compiler.
- `IS_PRIME` is a value alias: it has no address and cannot be assigned, but
  a run-time index such as `IS_PRIME[n]` reads the table.
- The sieve costs nothing when the program starts. The usual reason to do this
  is a table that is tedious to type and easy to get wrong by hand: CRC
  tables, character classes, sine approximations, lookup by opcode.

## Configuration as constants

A small number of value aliases describe the program, and the code adapts:

```dc example=config
```

- A table of structs, a table of text and a computed count are all value
  aliases. `([]const char)[3]` is an array of three text slices.
- `static if TRACE` keeps or drops the print. With `TRACE = false` the
  function body is empty in the compiled program, not an `if` that is skipped
  at run time.
- `clamp_slots(10)` uses another alias, `SLOTS`, inside the function: a
  compile-time call may read other compile-time constants.

## What it cannot do

CTFE evaluates a function on constants. These are refused with
"initializer for value alias is not a compile-time constant":

- reading or writing a global variable (its value only exists at run time);
- output, file or system access, and anything that calls such a function;
- reading outside an array, which is a run-time trap and so not a value;
- a function marked `@runtime`, which opts out of compile-time evaluation on
  purpose, for example because it reads a clock.

The compiler does not guess. If a value cannot be computed from constants,
it is not a constant, and the error names the alias, not the line that
failed inside the function: check the called functions one by one.

Two limits of the current compiler are worth knowing. A struct field whose
array length is a computed alias can get the wrong layout, so size arrays
from simple constants and use computed values only for tables and numbers. A
global array length cannot call a function.

## Value template parameters

A function template may also take *values* in its template list. They are
compile-time constants inside the function, so they can size arrays, bound
loops and feed `static if`:

```dc example=value_params
```

- `sum_first!(i32, 3)(...)` gives the type and the count; with several
  template parameters the list is parenthesized.
- `size_class!1()` passes one value, and a computed constant needs
  parentheses: `size_class!(sizeof(f32))()`.
- Each distinct value is another copy of the function, as for each type.
- Value parameters belong to functions. A struct template takes types, so use
  a named constant (`public using usize CAPACITY = 8;`) for a fixed size.

## Rejecting a bad request

`core::compile_error("message")` stops the compilation with your text. Put it
in the branch of a `static if` that only the bad case reaches:

```dc example=require
```

- `widen(7 as i64)` as a statement reports `require_small: this type is wider
  than 4 bytes` instead of silently truncating.
- Branches that are not taken are never analysed, so the error appears only
  for the types that reach it.
- The message is reported at the `compile_error` line inside the helper, so
  say in the text which function complains and what to change.
- Keep the check in a small function of its own. Every caller gets the same
  message, and the helper is easy to read.

## Common mistakes

- Putting run-time state in a table builder: a global, a print, a clock.
  The alias then stops being a constant.
- Writing a value alias as `usize SLOTS = 4;` (a variable). Without `using`
  it is an ordinary variable with storage that `static if` cannot read.
- Using `compile_error` unconditionally. It must sit in a `static if` branch
  that only the unwanted case reaches.
- Forgetting the parentheses around a computed template argument
  (`f!sizeof(T)()` is a syntax error, `f!(sizeof(T))()` is right).
- Expecting a runtime `if` to hide a `compile_error`. Compile-time actions run
  during analysis; a runtime `if` does not defer them.
