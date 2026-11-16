# Balanced brackets

The stack is an array plus a counter: pushing writes at `stack[depth]` and raises `depth`, popping lowers it. Every failure case has its own test: a closing bracket with an empty stack (closed before opened), a closing bracket whose partner is not on top (wrong partner or wrong order), and a non-empty stack at the end (never closed). The depth guard keeps the program inside the array even for hostile input; with bounds checks on, a missing guard would stop the program instead of corrupting memory.
