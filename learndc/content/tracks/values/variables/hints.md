## Hint 1

The compiler says `declared i32, initializer []const char`. A value written
in double quotes is text. A whole number is written without quotes.

## Hint 2

Two lines declare the sizes. After the build error is gone, compare the first
line of output with the task: the two sizes are in the wrong variables.

## Hint 3

In `src/main.dc`, the declarations should read `i32 width = 12;` and
`i32 height = 5;`. No other line needs to change.
