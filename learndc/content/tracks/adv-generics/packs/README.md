# Packs: any number of arguments

A generic function so far had a fixed number of type parameters: `T`, or `T`
and `U`. `println` takes any number of values, each of its own type, and it is
an ordinary DC function. The tool is the *parameter pack*: a trailing `T...`
in the template list stands for zero or more types at once.

## A pack of types and a pack of values

```dc example=count_total
```

- `(T...)` declares `T` as a *type pack*. Each call decides how many types it
  holds and which: `count(1, 2.5, 'c')` makes `T` the three types `i32`,
  `f64` and `char`.
- A parameter whose type is the pack, `T args`, becomes a *pack parameter*:
  one real parameter per type, in order.
- `sizeof...(T)` is the number of types in the pack, a compile-time `usize`.
  An empty call such as `count()` is valid and gives 0.
- `static for item in args { ... }` repeats its body once per element, and
  the compiler unrolls it: each copy sees `item` with that element's own
  type. That is why `item as f64` works for `i32`, `f64` and `u8` alike.

There is no runtime loop and no array of arguments. The compiler writes one
copy of the function per distinct list of types, and the "loop" is the code
it generates.

## Handing the elements on

Use an ordinary generic function inside the loop when each element needs
its own type-specific work, and re-expand the pack with `args...` to pass it
to another variadic function:

```dc example=show_all
```

- `show_one(item)` is called with a different `T` in each unrolled copy.
- `show_all(args...)` forwards the whole pack; the callee sees the same
  types.
- A function that returns `Status(Error)` and uses `?` inside `static for`
  works like any other: the first failure returns.

## Picking one element

An element has a constant index: `args.0` is the first, `args.1` the second.
The index must be known while compiling and in range:

```dc example=pick
```

Passing a single argument to `product_of_first_two` is a compile error,
`pack index 1 out of bounds for pack 'args' of length 1`. Extra arguments
are fine. When a function needs at least a certain number of elements, an
out-of-range index is the simplest way to say so, and a `compile_error`
(see the last page) gives the message you choose.

## Packs in structs

A struct template whose last parameter is a pack may have a *pack field*. It
expands to one ordinary field per type, with the layout of the same fields
written by hand:

```dc example=record
```

- `T... cells;` must be the last field and the struct may have only one
  pack field.
- `row.cells.0` reads the first cell; `static for field in t.items` visits
  each field of `std::tuple::Tuple`, the library's own pack struct.
- `sizeof(Row())` is 4 here: the pack contributes nothing and only `id`
  remains.
- A brace literal inside a generic function can splice a pack:
  `cells = {cells...}`.

## Rejecting bad arguments

There is no constraint syntax for "every element must be a number". A
function can check each element itself with a small helper that calls
`core::compile_error` when its own concept test fails, and it can reject an
empty pack. `compile_error` stops compilation with your message:

```dc example=guard
```

- `static if compiles(T a, T b) { ... } { } else { ... }` is the inline
  concept probe from the last lesson with an empty success branch.
- `average()` with no arguments, or `average(1, "x")`, would report your
  message instead of a failure somewhere inside the arithmetic.
- The error points at the `compile_error` line inside the function, so name
  the offending function in the message.

## Common mistakes

- Using a pack outside of expansion. `args` alone is not a value; use
  `static for`, an index such as `args.0`, or `args...`.
- Indexing past the end. `args.1` with one argument is an error at the call
  that instantiates the function.
- Mixing up the two sizes: `sizeof(T)` is bytes of one type, `sizeof...(T)`
  is the number of types in a pack.
- Writing a pack field before another field, or a second pack field in one
  struct.
