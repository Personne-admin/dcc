# Why this solution is built this way

The longest common subsequence has an optimal substructure: the LCS of two
sequences is either their first elements followed by the LCS of the rest (when the
first elements are equal) or the better of dropping the first element of one or the
other. Filling a table of all suffix pairs from the back turns that recursion
into two loops. The table is two dimensional but stored in one flat `Array(u32)`
with `i * width + j`, the same trick as any grid in DC, and the extra row and
column of zeros stand for "one sequence is empty".

The edit script is read off the table by walking forward from the start and
asking, at each step, which choice keeps the optimum reachable: keep equal
elements, otherwise remove from `a` if that does not lose LCS length and add from `b`
if it does. Because the rule is a strict tie-break, the script is unique, which is
why the harness can compare exact scripts. The property test checks the structure
instead: whatever the algorithm outputs must consume both sequences completely and
its `Keep` count must equal the independently computed LCS length.

The size guard is the memory-safety half of the task. A diff of two files of a
million lines would need a trillion cells; a typed `TooLarge` before any allocation
lets the caller decide what to do. The check divides instead of multiplying so that
the guard cannot overflow itself.

Generics and modules meet at the interface. `diff` knows nothing about text: it
needs only `==` on its elements. Text lines cannot be compared with `==`, so the
`lines` module interns them into integer ids first, a standard technique that also
makes the comparison inside the table loop cheap. Each module has one job and the
only shared language is `u32` ids, `Edit` values and `DiffError`.
