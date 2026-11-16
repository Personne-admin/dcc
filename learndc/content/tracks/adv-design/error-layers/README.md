# Layered errors

A small program can use one error type everywhere. A real one is built in
layers: a parser, an importer that uses the parser, an application that uses
the importer. Each layer fails in its own terms, and each caller needs to
know only what it can react to. This lesson designs the types, converts
between them with `?` and maps the final error to something the outside world
understands.

## One type per layer

```dc example=layers
```

- `ParseError` says what is wrong with a single field. It knows nothing about
  files, memory or the application.
- `ImportError` is the failure of the importing layer. It can be a parse
  problem, an allocation failure from `Array.push`, or its own `Duplicate`.
  The two wrapper variants are marked `@implicit_construction`, so a `?` on a
  `Status(ParseError)` or a `Status(AllocError)` builds the wrapper.
- `AppError` wraps `ImportError` and adds `NothingToDo`.
- Each function converts exactly one step with `?`: `add_item` turns a
  `ParseError` or an `AllocError` into an `ImportError`, `import_pair` turns an
  `ImportError` into an `AppError`. The code between the layers is only `?`.
- The caller at the top matches the nested errors from the outside in. The
  `match` must cover every variant of every level, so a new error is a compile
  error until each caller has decided what to do with it.
- `defer items.deinit()` releases the array on every one of those returns.

The compiler converts one step at a time. Propagating a `ParseError` straight
into a function that returns `Status(AppError)` is rejected with "cannot be
propagated into return type": the intermediate `ImportError` must be named, by
a function in between that returns it. That is not an obstacle but a design
rule: each layer has to decide what a lower-level failure means for it.

## Explicit conversion

`@implicit_construction` is a convenience for the obvious wrapper. When the
conversion has meaning, write it:

```dc example=explicit
```

- `BadInput(ParseError)` is an ordinary variant. The function checks the
  inner status, builds the wrapper itself and returns it. Build the value in a
  local variable (`AppError wrapped = ...; return wrapped;`) and return that.
- Conversions are the place for decisions: map several lower-level errors to
  one, drop detail the caller cannot use, or add information.
- `exit_code` is the *edge* of the program: the one function that knows the
  outside world's vocabulary (here, process exit codes) and translates the
  error enum into it. Inner layers never mention exit codes.

## Context travels in the error

A bare `BadNumber` does not say where. A variant can carry one payload, which
may be a small struct with whatever the layer knows:

```dc example=context
```

- `LineProblem` bundles the line number with the parse error that caused it.
  An enum variant takes one payload, so several values go into a struct.
- The layer that knows the line number adds it, because the parser below
  does not know it. Each layer adds the context that only it has.
- The report prints from the payload: `line 3: not a number`.

## Designing the types

- Name errors after the layer's operation, not its implementation:
  `ImportError::Duplicate`, not `ImportError::ArrayFindFailed`.
- Keep enums small and closed. Add a variant when a caller can act on it
  differently from the others.
- Do not collapse everything into the standard `Error` at the bottom. It is the
  right type for glue code that only propagates; a layer with its own
  vocabulary should have its own enum.
- Convert at layer boundaries, once. Do not smuggle a lower-level error
  through a layer unchanged.
- Translate to exit codes, messages or HTTP statuses only at the edge.

## Common mistakes

- Expecting `?` to chain two conversions. Return the intermediate type from an
  intermediate function, or convert by hand.
- Two variants with the same payload type both marked
  `@implicit_construction`: the compiler cannot choose, so each payload type
  has at most one implicit variant.
- Matching on the wrapper and ignoring the inner error: the cause is lost
  and the message becomes "something failed".
- Using the error enum as a flag and returning `Status::Ok` after printing a
  message. The caller must see the failure.
- Returning a payload variant constructor straight from a function whose
  result is `Status(E)`: build it in a variable first, then return the
  variable.
