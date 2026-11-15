# Copies and pointers

Last lesson, `award(hero, 50)` could not change `hero` by itself: the
function got a copy and had to hand the changed copy back. This lecture
explains why, and shows the other tool: a *pointer*, which lets a function
change the caller's own variable.

## A function gets a copy

Assigning a value to a new variable, or passing it to a function, copies it:

```dc example=copy_demo
```

`b` is a separate struct with its own `value`, so changing it leaves `a`
alone. In the same way `bump_copy` bumps its own copy of `a`; the original
is still 5 afterwards. For numbers, text and structs alike:

```text
main                          bump_copy
+---------+                   +---------+
| a  5    |  -- copied -->    | c  5    |  becomes 6
+---------+                   +---------+
   still 5 after the call
```

## A pointer is an address

Every variable lives at an address in memory. A pointer is a value that
holds such an address. For a type `T`, the type `T*` means "pointer to a
`T`":

- `&n` produces the address of the variable `n`.
- `*p` follows the pointer `p` to the variable it points at. You can read
  `*p` or assign to it.

```text
n   +-------+        p   +-----------+
    |   5   | <--------- | address   |      i32* p = &n;
    +-------+            +-----------+      *p = 9;   changes n
```

## Writing through a pointer

When a function takes a pointer, the caller passes `&variable`, and the
function changes the caller's variable with `*`:

```dc example=pointer_demo
```

Nothing is copied except the small address, so `add_one` really changes
`n`. This is the way to write a function that updates a value in place.
Forgetting the `&` at the call is a compile error (`no matching call`), not
a silent bug.

## Structs and pointers

For a pointer to a struct you do not need `*`: the dot works through the
pointer, so inside `void add(Counter* c, i32 amount)` you write
`c.value += amount;`. A pointer parameter is the way to say "this function
changes its argument". Use a plain value parameter when the function only
reads.

## Dot syntax is only a spelling

If a function's first parameter is a `Counter` or a `Counter*`, you may write
the call with the value in front of a dot. That is the same call, spelled
differently, and for a pointer parameter the `&` is added for you:

```dc example=dot_syntax
```

`add(&c, 3)` and `c.add(4)` both change `c`, and `doubled(c)` equals
`c.doubled()`. The dot form is called UFCS. It does not give the struct any
hidden powers: it only reads left to right, so a chain such as
`hero.hit(30)` says who is acted upon first.

## Common mistakes

- Writing a function that changes a by-value parameter and expecting the
  caller to see it. Either return the new value or take a pointer.
- Forgetting `&` when calling a function that takes a pointer, or writing
  `*` where the dot already works through the pointer.
- Believing that `c.add(4)` is a different mechanism from `add(&c, 4)`. It is
  the same function.
