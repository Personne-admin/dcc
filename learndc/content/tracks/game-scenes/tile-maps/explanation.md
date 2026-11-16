# Why this works

The text is only the authoring format. `parse` is the boundary where untrusted text becomes trusted data: after it succeeds
every cell is 0 or 1, the start is known, and the rest of the game never looks at characters again. Doing the validation
there, and returning `false` instead of crashing, means a typo in a level file is reported once and early.

`is_wall` treats the outside of the map as wall, which turns a whole class of bounds checks into one rule. The collision
code of the next exercise can ask about any tile, including ones past the edge, and gets a sensible answer.

The camera is just a subtraction. World coordinates (where things are in the level) and screen coordinates (where they
appear) are different, and the camera is the offset between them. Keeping both ideas separate is what lets a level
be larger than the window later: the world and the update logic never change, only what `draw` subtracts.
