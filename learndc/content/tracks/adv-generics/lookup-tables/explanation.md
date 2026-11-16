# Why this works

`using u8[128] CLASSES = build_classes();` asks the compiler to run
`build_classes` and keep the returned array as a constant. The 128 branches of
the rules are paid once, while compiling; the finished program has a
128-byte table and `class_of` is one bounds check and one load. That is the
reason to build tables in the compiler: the logic stays readable (rules
written as code), and the run-time form is the fastest possible one.

The builders are plain functions with loops, a local array and assignments.
CTFE may run anything of that kind. What it forbids is the dependence on
run-time state: a global, a print, a clock. That restriction is also what
makes the table trustworthy, because a function that qualifies produces the
same table every time.

`class_of` and `rot13` still check `code >= 128`: the alias has exactly 128
entries, and a run-time index outside of it would be out of bounds. The
table makes the common case cheap, the guard keeps the rest defined.

The harness does not test a few samples. It walks all 128 codes and compares
the table with the rules written a second time, and it checks that rot13
twice returns the original character, a property that holds only when every
entry is right.
