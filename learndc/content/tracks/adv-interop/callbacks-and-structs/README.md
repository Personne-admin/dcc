# Callbacks and structs

Two more things cross the C boundary besides numbers and text: functions
and records. A C library sorts, searches, schedules or notifies by *calling
back* a function you hand it, and it exchanges records (`struct tm`,
`div_t`) whose layout both sides must agree on.

## A function is a pointer

DC has function pointer types, written `ReturnType(*)(Params)`, and a name
for one is usually a `using` alias:

```dc
using Compare = i32(*)(const void*, const void*, void*);
```

The name of a function converts to such a pointer, and so does a lambda that
captures nothing. A function pointer is a bare address. It carries no
captured variables, which is exactly the C model: C code can only call it with
the arguments *it* chooses.

## What a C library does with your callback

This program is a small version of what `qsort_r` does inside libc. It
knows nothing about the element type: it only knows the block size, and asks
your function to compare two blocks:

```dc example=callback_sort
```

Look at the three conventions it relies on:

- **Everything is `void*`.** The sorter cannot know what it is sorting, so
  the comparator receives `const void*` and casts it back with `as const i32*`.
  Plain pointers convert to `void*` only through an explicit `as`.
- **The comparator returns a sign.** Negative, zero or positive, like
  `strcmp`. Anything else (for example subtracting two large `i32` values,
  which can overflow) is a bug waiting for the wrong input.
- **State travels in a `void*`.** Because a function pointer cannot capture,
  C APIs give you an extra argument that is passed back to the callback.
  Here it points at a `Counter`; in libc it is the last parameter of `qsort_r`.

A callback may be called many times and in an order you do not control. It
must not store pointers to its arguments, and it must not raise an error
that C cannot see: there is no `?` through C frames. Report problems through
the context instead.

## Records

When C gives you a pointer to a struct, or returns one by value, DC needs a
struct with the same fields in the same order, so that every offset agrees.
You already know the rules: fields are laid out in order with padding for
alignment. C compilers on 64-bit Linux follow the same rules, so a faithful
copy of the declaration has the same layout.

```dc example=c_struct
```

`struct tm` from `<time.h>` is 56 bytes with the eleven fields above on
64-bit Linux with glibc. Printing `sizeof` and `offsetof` and comparing with
the C header (or with a C program's output) is the cheapest test that the
declaration is right. If a number differs, a field has the wrong type or
is missing.

Passing the struct in either direction works like any other argument:

```dc
@nomangle extern Tm* gmtime_r(const i64* seconds, Tm* result);
@nomangle extern DivResult div(i32 numerator, i32 denominator);
```

`gmtime_r` fills the `Tm` you give it (an out parameter), and `div` returns
a small struct by value; DC follows the C rules for both.

## Common mistakes

- Declaring a callback with the wrong parameter types. The link succeeds,
  and C calls it with arguments in registers your function does not expect.
- Returning `a - b` from a comparator.
- Forgetting that a function pointer has no captured variables: use the
  `void*` context.
- Declaring only the fields you need in a C struct. The declaration must
  cover the whole layout up to the last field you read, and the whole size if
  C writes into your struct.
