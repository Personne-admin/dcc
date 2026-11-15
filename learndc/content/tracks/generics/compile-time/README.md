# Compile time

Some answers do not depend on the data a program reads: the number of slots
in a table, the size of a struct, whether a type is a float. DC lets you ask
the compiler for those answers while it compiles. The result is baked into
the program, so nothing is computed when it runs, and a wrong answer is a
compile error instead of a crash.

## Named constants

A *value alias* gives a name to a constant value. Its type comes first, then
the name and the value:

```dc example=constants
```

- `using usize SLOTS = 4;` is not a variable. It has no storage and cannot be
  assigned to; every use is replaced by its value.
- The value may be computed from other constants: `24 * 60 * 60`.
- Because the value is known while compiling, it can size an array, even in
  a struct field: `i32[SLOTS * 2]`.

## Layout: sizeof, alignof and offsetof

`sizeof(T)` is the number of bytes a value of type `T` occupies; `alignof(T)`
is the address multiple it must sit on; `offsetof(S, field)` is where a field
starts inside a struct. All three are `usize` constants.

The CPU reads a 4-byte value fastest, and in some cases only, from an
address that is a multiple of 4. The compiler therefore keeps every field on
a boundary that matches its own size and fills the gaps with unused
*padding* bytes. The struct as a whole is padded at the end so that arrays of
it stay aligned. The order of the fields decides how much padding is needed:

```dc example=layout
```

`Loose` puts a 1-byte field, then a 4-byte one, then a 1-byte one. Offsets
are 0, 4 and 8: three padding bytes after `flag`, and three after `tag` to
round the size up to a multiple of 4, for 12 bytes in total. `Tight` has the
same fields with the large one first: 0, 4 and 5, with two bytes of padding
at the end, for 8. The same data takes a third less memory. A rule of thumb:
list fields from largest to smallest.

## Calling functions at compile time

A value alias may use the result of an ordinary function. The compiler runs
the function while it compiles (*compile-time function evaluation*) and uses
the number:

```dc example=compile_call
```

`factorial(10)` is computed by the compiler, and `FACTORIAL_10` becomes a
literal. `triple(2)` inside `i32[...]` sizes the array. The function is a
normal function that can also be called at run time; it only has to be simple
enough for the compiler to run, so no input and no printing.

## Choosing code at compile time

`static if` is an `if` the compiler decides while compiling. Only the branch
that applies is compiled; the others are dropped. Inside a generic function
it can ask about `T`:

```dc example=static_if
```

- `T == i32` compares types, and the conditions may be joined with `||`.
- `sizeof(T) == 1` compares a constant that depends on the type.
- Each call, such as `kind_of!i32()`, compiles only the branch for its own
  type. The branches that do not apply may even contain code that would be
  invalid for that type.
- A function with no value parameter that depends only on `T` has empty value
  parentheses and is called with an explicit type: `kind_of!i32()`.

## Common mistakes

- Writing `usize SLOTS = 4;` (a variable) where a constant is needed.
  A `static if` condition and a struct field's array length must be known
  while compiling, so they need a value alias.
- Expecting the field order of a struct to be free. Padding is real memory:
  sort the fields from large to small.
- Using a `const` global as an array length. Use a value alias
  (`using usize SLOTS = 4;`); it is the form that is always evaluated in
  time.
- Mixing a module constant and a type test inside one `static if` condition.
  Nest two `static if` blocks instead.
