# Why this works

A call to `kind` first discards every overload whose concept the argument
type fails. Whatever is left competes as usual, and two survivors with equal
conversions make the call ambiguous. Concepts are never ranked, so the only
way to select an overload is to make the sets of types disjoint. The four
concepts partition all types: `Numeric` types split into `Integer` and
`Float` by one operation, and the remaining types split into `Sequence` and
`Other`.

The probes are deliberately small. `a << 1` compiles for integers only, so
it separates them from floats without naming a single type: a new integer
type added to the language later would land in the right overload. This is
the point of concepts over comparing types with `T == i32 || T == i64`.

`Other` is defined by what it excludes. That is a useful pattern for the
fallback overload, and it shows that a negation belongs inside a concept
(`!Numeric(T) && !Sequence(T)`), not in the `if` clause of the function.
`char` and `bool` end up there because neither supports `+`.
