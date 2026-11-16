# Dispatch on what a type can do

`kinds.dc` has one function, `kind`, in four overloads. Each overload names
a concept in its `if` clause and returns a word describing the type:

| overload | returns | types |
|---|---|---|
| `Integer` | `integer` | `i8` .. `u64` |
| `Float` | `float` | `f32`, `f64` |
| `Sequence` | `sequence` | anything with a length and an element at index 0 |
| `Other` | `other` | everything else: `bool`, `char`, structs, enums, pointers |

`Addable`, `Orderable` and `Numeric` are already written. The concepts
`Integer`, `Float`, `Sequence` and `Other` are placeholders that accept
every type, so every call is ambiguous and the project does not compile yet.
Rewrite the four concepts so that each type satisfies exactly one of them.

Hints about the probes:

- Integers and floats can both be added and compared. What can an integer do
  that a float cannot? Add a small concept for it (say `Shiftable`) and use it
  and its negation.
- A `Sequence` has `v.len` and `v[0]`.
- `Other` is everything that is neither numeric nor a sequence.
- Write a negation inside a concept body, for example `!Shiftable(T)`.

`main.dc` prints a table and must not change:

```text
u8: integer
f64: float
char: other
bool: other
Point: other
text: sequence
i32 slice: sequence
pointer: other
```

## Acceptance criteria

- The project builds with `dpm build` and `main.dc` is unchanged.
- The table above matches exactly and the exit code is 0.
- The harness checks pass for types that `main` does not print.
