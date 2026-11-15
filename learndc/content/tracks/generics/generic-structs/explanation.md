# Why this works

`Stack(T)` is a recipe. When `main` writes `Stack(i32)`, the compiler
writes a struct with `i32[8] items` and when it writes `Stack(char)`, one
with `char[8] items`. The functions follow the same recipe: `pop(T)` is
compiled once for `i32` and once for `char`, and `T` is deduced from the stack
passed in.

The whole design rests on one invariant: the first `len` slots of `items` are
valid and the rest are unused. `push` writes at `len` and then grows it;
`pop` shrinks `len` first and then reads the slot that just stopped being in
use, which is the old last element. `peek` reads that same slot without
touching `len`. The slot is not erased on pop, and nobody can reach it
through the API, so there is no need.

Returning `Optional(T)` makes the empty case part of the type. The caller
cannot forget it: `unwrap_some()` is only safe after `is_some()`. `push`
returns `bool` for the opposite problem, a full stack, and refuses before
changing anything so a refused push leaves the stack exactly as it was.

`size` and `peek` take `const Stack(T)*`: a promise not to change the stack,
which the compiler enforces.
