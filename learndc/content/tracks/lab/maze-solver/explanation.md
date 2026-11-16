# Why the search finds the shortest route

Breadth-first search visits cells in order of distance because the queue is
first in, first out: everything one step from the start is queued before
anything two steps away, and so on. The first time a cell is reached is
therefore along a shortest route, and writing its distance at that moment
(`dist[n] = dist[cell] + 1`) both records the answer and marks the cell as seen.
Marking at queue time, not at visit time, is what keeps a cell from being
queued twice and what makes the loop end: every cell enters the queue at most
once, so the work is proportional to the size of the maze.

The queue is an `Array(usize)` with a `head` counter instead of a real queue.
Removing the front of an array would move every element; leaving visited
entries in place costs only memory proportional to the maze and keeps each step
constant time.

`find_path` needs no parent pointers. A distance array that comes from a
breadth-first search has a useful property: every reachable cell except the
start has a neighbour exactly one step closer. Walking from the exit towards
smaller distances therefore reconstructs a shortest route, and the fixed
neighbour order (up, right, down, left) makes the choice between equal routes
deterministic. The route comes out backwards, so it is reversed in place.

Errors are typed because they are different situations the caller may handle
differently: a plan without `S` or `E` is a bad drawing, `NoPath` is a real
answer about the maze. The helpers that can run out of memory report it with
`?` and `@implicit_construction` turns it into `MazeError::Alloc`. All
temporary arrays are owned by the function that made them and released by
`defer`, on every return.
