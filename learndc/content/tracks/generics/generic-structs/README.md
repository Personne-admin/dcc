# A generic stack

A *stack* hands elements back in reverse order: the last one in is the first
one out. It is useful for undo lists, for reversing things and for matching
brackets. The element type should not matter, so the stack is a *struct
template*: a struct with a type parameter in parentheses after its name.

```dc
public struct Stack(T) {
    T[CAPACITY] items;
    usize len;
}
```

`Stack(i32)` and `Stack(char)` are two different, fully typed structs the
compiler writes from this one description. `items` is a fixed array with room
for `CAPACITY` values and `len` counts how many are in use. (`CAPACITY` is a
named compile-time constant, introduced in the next lecture; here it simply
means 8.)

Functions on the struct are generic in the same `T`. The type parameter list
comes first and the struct pointer repeats it, `Stack(T)*`; `T` is deduced
from the stack you pass:

```dc
public bool push(T)(Stack(T)* self, T value) { ... }

Stack(i32) numbers = {};
numbers.push(5);          // same as push(&numbers, 5); T is i32
```

Because the first parameter is a `Stack(T)*`, the call `numbers.push(5)` reads
like a method.

## Your task

`src/stack.dc` has the struct and a working `push`, which refuses a ninth
value by returning `false`. `src/main.dc` also uses `pop`, `peek` and `size`,
which are missing, so the project does not build yet. Add them after
`push`, all generic over `T`:

1. `pop(T)(Stack(T)* self)` returns `Optional(T)`: the last value, removed,
   or `Optional::None` when the stack is empty.
2. `peek(T)(const Stack(T)* self)` returns `Optional(T)`: the last value
   without removing it, or `Optional::None`.
3. `size(T)(const Stack(T)* self)` returns the number of values as `usize`.

When all three work, the program prints:

```text
accepted 8 of 10, size 8
popped: 8 7 6
top is 5, size 5
reversed: kcats
empty pop is none: true
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The output above matches exactly and the exit code is 0.
- The harness checks pass for `Stack(f64)` and `Stack(u8)`, including empty
  and full stacks.
