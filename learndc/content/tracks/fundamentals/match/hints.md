## Hint 1

The error says `match expression not exhaustive: missing case` and names a
variant of `Light`. A `match` must have an arm for every variant of the enum
it inspects.

## Hint 2

Add the missing arm to `action` with the text `"Slow down"`. When the project
builds, look at the Yellow line of the output: `seconds` returns the wrong
number for it, because the `_` arm answers for every light except Red.

## Hint 3

In `src/traffic.dc`, add `Light::Yellow => "Slow down",` to `action`. In
`seconds`, replace `_ => 25,` with `Light::Yellow => 5,` and
`Light::Green => 25,`.
