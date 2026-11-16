## Hint 1

Overload selection removes every overload whose concept fails and then needs
exactly one left. So each type must satisfy exactly one of `Integer`,
`Float`, `Sequence` and `Other`. Start with the two ends: what do sequences
have that numbers do not, and what is left when neither applies?

## Hint 2

`Sequence(T) = compiles(T v) { v.len; v[0]; };`. For the numbers, add
`Shiftable(T) = compiles(T a) { a << 1; };` (integers only). `Integer` is
`Numeric(T) && Shiftable(T)`; `Float` is numeric and not shiftable;
`Other` is not numeric and not a sequence.

## Hint 3

```
public using Shiftable(T) = compiles(T a) { a << 1; };
public using Integer(T) = Numeric(T) && Shiftable(T);
public using Float(T) = Numeric(T) && !Shiftable(T);
public using Sequence(T) = compiles(T v) { v.len; v[0]; };
public using Other(T) = !Numeric(T) && !Sequence(T);
```
