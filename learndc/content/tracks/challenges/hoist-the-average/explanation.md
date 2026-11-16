# Optimize: do not recompute inside the loop

A computation that does not depend on the loop variable is a loop invariant; calling it inside the loop repeats the same work n times. Moving it in front of the loop turns n times n reads into n plus n. Counting reads makes the improvement visible and exact (10 reads for five values instead of 30), and the harness bound of 2n lets any correct hoisting pass.
