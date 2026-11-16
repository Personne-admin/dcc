# Concepts: what a type must be able to do

A generic function accepts any `T`, but its body can only work for the types
that support the operations it uses. Without a limit, the mistake shows up
deep inside the body, when some caller finally passes the wrong type. A
*concept* writes the requirement down once, so that the compiler can check it
at the call and explain which operation is missing.

## A concept is a probe

A concept is a named list of expressions that must compile. It is declared
with `using`, takes the type parameter in parentheses and lists the values it
needs:

```dc example=requirement
```

- `compiles(T a, T b) { a + b; }` says: declare a value `a` and a value `b`
  of type `T` and try to compile the body. If every statement compiles, `T`
  satisfies the concept. Nothing in the body runs.
- `if Addable(T)` after the parameter list attaches the concept to the
  function. A call for a type that fails the probe is rejected before the
  function body is looked at.
- Every statement in the probe is an independent requirement. The diagnostic
  for a failed concept lists each requirement that did not compile, with the
  real compiler message for it.

The requirement is about syntax and types, not meaning: `Addable` is also
satisfied by a type for which `+` compiles but does something surprising.

## Combining concepts

A concept may use other concepts with `&&` and `||`, and `!` negates one.
Giving each idea its own small name keeps constraints readable:

```dc example=composed
```

- `Numeric(T)` is satisfied when both smaller concepts are.
- Two overloads of `describe` can have the same signature when their
  constraints cannot both hold. For each call exactly one is viable.
- The negation is written inside a concept (`NotNumeric`) and the function
  names that concept in its `if`.
- A struct with two `i32` fields has no `+`, so it takes the second overload.
  So does `bool`.

## Overloads are not ranked

The compiler does not prefer the "more specific" constraint. If two
overloads both accept a type, the call is ambiguous, even when one concept
clearly includes the other. Design constraints that exclude each other, as
`Numeric` and `NotNumeric` do, or give the functions different names.

## Branching inside one function

Sometimes two cases differ by a few lines, not a whole function. `static if
compiles(...) { probe } { then } else { otherwise }` asks the same question
inside the body, and the compiler keeps only the matching branch:

```dc example=probe
```

- The probe has its own declarations: `compiles(T v) { v.len; }` introduces
  `v` of type `T`, and the braces after it hold the requirement.
- The branch that does not apply is not compiled for that type. In the
  second branch `value.len` would be an error for `u16`, and it is not an
  error because it is dropped.
- A fixed array such as `i32[3]` has no `.len`, so it takes the `else`
  branch. This example hands over a slice instead.

## Constraining a struct

A struct template can carry the same `if`. The check runs wherever the type
is written, including inside other generic code:

```dc example=struct_constraint
```

`Range(bool)` or `Range(Point)` would be rejected with "template constraint
not satisfied for `Range`". A function that takes a `Range(T)` repeats the
constraint, because the function is a separate template.

## Common mistakes

- Probing with the wrong operations. A concept that only tests `a < b` accepts
  a type whose function body then uses `a + b`, and the failure moves back
  inside the body. Probe everything the body uses.
- Expecting the best match. Two overloads whose constraints overlap give
  "ambiguous call"; they are not ordered by specificity.
- Writing the negation directly in a function's `if` clause. Define a concept
  whose body is the negation (`using NotNumeric(T) = !Numeric(T);`) and use
  that name.
- Forgetting that a probe never runs. `compiles(T a) { a / 0; }` is satisfied
  by integers: the probe only checks that the expression compiles.
- Using a constraint instead of reading the diagnostic. The note under
  "template constraint not satisfied" names the concept, the type and each
  failed requirement: read those lines first.
