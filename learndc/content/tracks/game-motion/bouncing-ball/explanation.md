# Why it works

The motion is `position += velocity * dt`, and a collision is a correction applied after it. The two axes do not influence each other, which is why the code for x and y is the same block twice: a wall flips the one velocity component that points into it, and the other component carries on. At a corner both flip, which turns the velocity around completely.

Position correction is as important as the flip. If you only flipped the velocity, the ball would still be beyond the wall at the start of the next step and, moving back toward the box slower than it had come, could be detected as outside again and flipped back. That is the classic "ball stuck in the wall" jitter. Mirroring the overshoot puts the ball where it would be had it travelled to the wall and returned, which is also inside the box (as long as one step does not carry the ball farther than the whole box, which is what small steps guarantee).

Reflection changes the direction of the velocity, never its length: the long-run check confirms that `vx * vx + vy * vy` is the same after thousands of bounces. The return value is not needed by the physics. It is the kind of event count a game uses for sound effects, scores or a debug display, and it gives the check something precise to look at.
