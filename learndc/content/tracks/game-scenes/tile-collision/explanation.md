# Why this works

A box is blocked exactly when one of the tiles it overlaps is a wall, and the overlapped tiles form a small rectangle of
columns and rows, so the test is two short loops. The `0.001` makes "touching" mean "not overlapping", which is what lets a
player stand flush against a wall and still be allowed to move along it.

Moving one axis at a time is the trick that makes tile movement feel right. If you tested the diagonal step as a whole, a
box pushing into a wall would stop completely. With separate axes, the blocked axis is resolved against the wall and the
free axis keeps going, which is sliding. The order (x then y) can matter in extreme corners but is invisible at normal speeds.

Snapping flush replaces "undo the move" because the undo leaves a gap of up to one step, and a faster player would leave a
bigger gap. The snapped position is the wall's own edge, so the result is the same at any speed or frame rate. The method needs
steps smaller than a tile, which a game guarantees by limiting the time step; larger steps would need to be split.

Negative coordinates deserve their own care because truncation toward zero maps -3 to column 0, a wall-free tile.
Handling them before the division keeps the "outside the map is a wall" rule of `is_wall` intact.
