# Why the solution works

Each `{}` consumes the next argument, so the number of holes and the order of
the arguments must agree with what you want to see. Text arguments are
printed as they are; numbers are printed in decimal.

`print` does not add a newline. The line `Items: 2 x tea, 1 x cake` is
therefore written by two calls: `print("Items: ")` leaves the cursor at the
end of the label, and the `println` that follows continues on the same line
and ends it.

Every call still ends in `?`, which passes a failed write on instead of
ignoring it.
