## Hint 1

Work through the stages in order. Each one is a few functions with its own check, and a later stage can rely on an earlier one: movement needs the level, scenes need movement and interaction, drawing needs the resources.

## Hint 2

Almost everything has been practised in this path: the grid index `row * COLS + col`, the scripted press `down && !prev`, the all or nothing load, the flush collision (provided here), and the frame from elapsed milliseconds. The stage hints say which one each stage reuses.

## Hint 3

Start every function with the failing check's hint, make that one thing pass, and run `learndc check` again. If a stage behaves oddly in the window, reproduce it with a scripted input in your head first: all the logic takes plain structs.
