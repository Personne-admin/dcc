## Hint 1

The diagnostic says `unknown path temperature::to_fahrenheit`, although the
import is there. Names declared in a module are private unless something says
otherwise.

## Hint 2

The keyword that exports a declaration is `public`, written first on the
declaration. The new function needs it too, otherwise `main` will hit the same
error again.

## Hint 3

In `src/temperature.dc` write `public i32 to_fahrenheit(...)`. Below it add
`public bool is_freezing(i32 celsius)` returning `celsius < 0`. In
`src/main.dc` add `println("-5 C freezing: {}", temperature::is_freezing(-5))?;`
before `return Status::Ok;`.
