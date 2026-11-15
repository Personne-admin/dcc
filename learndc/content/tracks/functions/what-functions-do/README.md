# What functions do

You have already used one function in every program: `main`. This lecture
shows how to write your own, so that a piece of work gets a name and can be
used as often as you like.

## Why functions exist

Imagine a program that needs the volume of three boxes. Without a function
you would write the multiplication three times and, one day, fix a mistake in
two of them and forget the third. A function writes the recipe once and gives
it a name:

```dc example=repeat_work
```

`box_volume` is *defined* once and *called* three times, each time with
different numbers. If the recipe ever changes, there is exactly one place to
change it. A good function also gives a name to an idea: `box_volume(2, 3, 4)`
reads better than `2 * 3 * 4`.

## The parts of a signature

The first line of a function is its *signature*. It tells the compiler and
every reader how to use the function without looking inside:

```text
  return type    name          parameters
       |          |                |
       v          v                v
     i32      box_volume(i32 width, i32 height, i32 depth) {
         return width * height * depth;      <- the body
     }
```

- The *return type* is the type of the value the function hands back:
  `i32`, `bool`, `f64`, `[]const char`, and so on. A function that hands
  back nothing is declared `void`.
- The *name* is how you call it. Use lowercase words joined by underscores.
- The *parameters* are a list of `type name` pairs, separated by commas.
  Inside the body each parameter is an ordinary variable.
- The *body* is the block between the braces. `return value;` ends the
  function at once and delivers the value, which must match the return type.

## Calling a function and the call flow

To call a function write its name and one argument per parameter, in the same
order. The call is an expression: it *becomes* the returned value, so you can
store it, print it or pass it to another function.

```dc example=call_flow
```

When `main` calls `twice_plus_one(5)`, `main` waits. `twice_plus_one` runs, in
the middle it calls `twice(5)` and waits in turn. Each `return` sends a value
back to whoever called:

```text
main              twice_plus_one            twice
 |  call (5)             |                    |
 | --------------------> |   call (5)         |
 |                       | -----------------> |
 |                       |       return 10    |
 |                       | <----------------- |
 |      return 11        |
 | <-------------------- |
```

The arguments of a call are evaluated first, from the inside out. In
`twice_plus_one(twice(3))`, `twice(3)` is 6, so the outer call receives 6.

## Parameters are copies

A parameter starts as a copy of the argument. Changing the parameter inside
the function does not change the caller's variable:

```dc example=copies
```

`score` in `main` is still 50. The function changed its own copy and returned
the result; the caller decides what to do with it. To use a result, store it
or print it, as `main` does with `better`.

## A word about the next lessons

The exercises of this chapter keep their functions in a second file, such as
`numbers.dc`, so that a checker can call them without running `main`. The
starter `main.dc` begins with `import numbers; using numbers::*;`, and
functions in the other file are marked `public`. Think of it as scaffolding:
`main.dc` uses `numbers.dc`. The Modules chapter explains how that works, and
until then you only have to leave those lines as they are.

## Common mistakes

- Forgetting `return`. A function with a return type must return a value on
  every path; the compiler reports `missing return in function`.
- Calling with the wrong number of arguments, or with an argument of another
  type. The compiler reports `no matching call` and lists the signature it
  expected.
- Calling a function and throwing the result away. `square(4);` computes 16
  and does nothing with it. Store it in a variable or print it.
- Expecting a changed parameter to change the caller's variable. It does not.
