# Why it works

The overlap test is the observation that two rectangles are *separated* if there is a gap on either axis, and overlap when there is none. The four comparisons say "no gap on x" and "no gap on y"; the strict `<` makes touching count as a gap of zero, so resting contact is not a collision.

Knowing *that* they overlap does not say *where* the mover came from, and the answer is hidden in the overlap itself. A mover that entered through the left side of a wall overlaps it by a thin sliver in x but by the whole height in y. The smaller depth is the side of entry, so pushing out along it undoes the last bit of motion and nothing more. Choosing the side by comparing centers then tells which of the two directions on that axis is outward.

Setting the velocity from the side instead of negating it blindly matters when `resolve` runs on consecutive frames: a mover that was pushed out and is moving away must not be turned back toward the wall because it still overlaps for an instant. And since only the penetrating axis is corrected, a mover pressed against a wall keeps sliding along it, which is what players expect in a game. In `main.dc` the same trick is applied in two passes, x first and then y, which avoids the corner cases where a diagonal move touches two walls at once.
