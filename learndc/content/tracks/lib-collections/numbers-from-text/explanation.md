# Why the checks come in this order

`accumulate_digits` reads as much as it can, so each of its answers means
less than it seems. Success means "some digits were read", not "the text is a
number". The `cursor == text.len` test closes that gap and is what rejects
`12ab`; without it the program would quietly parse 12 and ignore the rest.

The sign is not part of the digit reader because formats disagree about
signs, separators and bases: JSON forbids `+`, TOML allows underscores. That is
also why `signed_from_magnitude` exists as a separate step: the asymmetry of
two's complement (there is one more negative number than positive) is
handled in one place instead of in every parser.

`ParseFail` keeps library details out of the rest of the program. `main`
and `report` know three outcomes; whether the cause was `NoDigits` or a
stray letter does not matter to them. In `sum_line` the `?` on `parse_i64`
returns the same `ParseFail` unchanged, so the first bad token ends the line
and the total computed so far is discarded.
