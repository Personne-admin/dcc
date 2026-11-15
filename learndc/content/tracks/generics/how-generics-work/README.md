# How generics work

You have written `larger(i32 a, i32 b)` and wished it worked for `f64` as
well. Copying the function for every type is slow to write and easy to get
wrong: fix a bug in one copy and the others keep it. A *generic* function
is written once with a placeholder type, and the compiler produces the real
functions for you.

## One function, many types

A type parameter goes in its own parentheses between the name and the
ordinary parameters. By convention it is called `T`.

```dc example=one_function
```

- `larger(T)(T a, T b)` reads: "for some type `T`, take two `T` and return
  a `T`".
- The body may use `T` anywhere a type is allowed: as a parameter type, a
  return type or the type of a local variable.
- The body may only use operations that every `T` you call it with has. `>`
  works for numbers and characters, so all three calls compile.

## How T is chosen

The compiler reads `T` off the arguments. `larger(3, 9)` gives `T = i32`,
`larger(2.5, 0.5)` gives `T = f64`. It can also see through a slice, so a
parameter `[]const T` binds `T` to the element type:

```dc example=deduction
```

- A fixed array or a slice of `i32` makes `T` equal to `i32`.
- If the compiler cannot decide, or you want a different type than the
  arguments suggest, write the type yourself with `!`: `middle!i32(numbers)`
  or `larger!u8(200, 100)`.
- Two parameters that share one `T` must agree. `larger(a, b)` with an
  `i32` and an `f64` has no valid `T`, and the compiler reports that no
  candidate matches.

## One copy per type

A generic function is not slower and not "dynamic". For every type it is
used with, the compiler writes out an ordinary function with `T` replaced
by that type. This is called *monomorphization*: one copy for each type. The
copies are separate, so each one has the size and the operations of its own
type:

```dc example=one_copy_each
```

Three calls with three types produce three functions. Two calls with the
same type share one copy. A generic function that nobody calls produces no
code at all.

## Limiting T

Without a limit, the body is only checked when a type actually reaches it,
and the error then points into the function. A *concept* names what a type
must be able to do, and `if Concept(T)` on the function turns a wrong type
into a clear error at the call:

```dc example=constrained
```

`Addable(T)` is satisfied when `a + b` compiles for two values of `T`. A call
with `bool` or a struct is rejected with "template constraint not satisfied"
and a note naming the concept.

## Common mistakes

- Using `T` without declaring it. `T larger(i32 a, i32 b)` has no `(T)`
  list, so `T` is an unknown type.
- Mixing types in one call. With `larger(T)(T a, T b)`, an `i32` and an `f64`
  cannot both be `T`. Convert one argument with `as`, or give `T` explicitly.
- Passing a bare number next to a small-type array. A literal defaults to
  `i32`, so `above(bytes, 100)` with a `u8` array finds two different `T`
  and the diagnostic complains about the array. Write `above!u8(bytes, 100)`.
- Expecting an operation to exist for every `T`. A body that uses `+` fails
  for `bool`; add a constraint so the failure appears at the call.
- Thinking one copy serves all types. Each type gets its own compiled
  function with its own size and instructions.
