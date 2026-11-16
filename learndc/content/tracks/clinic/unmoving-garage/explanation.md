# Root cause: The garage that never fills

`void enter(Garage g)` receives a copy of the struct. The three calls decrement `free` and increment `parked` of three short-lived copies, so `main` never sees anything. `leave` works because its parameter is `Garage*`: the function writes into the caller's struct.

The method: when a value changes inside a function but not afterwards, ask whether the function sees the original or a copy. A quick experiment proves it, print the field inside the function and in the caller. Inside it shows the new value, outside the old one.

The same copy bites with `board(Garage g)`, but there a copy is exactly right: it only reads. Take a pointer when the function must change the caller's value, and a value when it only reads a small struct.
