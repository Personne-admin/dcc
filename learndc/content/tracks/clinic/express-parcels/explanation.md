# Root cause: Express at the local rate

`fee` ended with `_ => 4 + kilos`. Local was the only zone without its own arm, so the wildcard looked like a harmless spelling of "local". When `Express` joined the enum the match still compiled, because `_` makes every match exhaustive.

Without the wildcard the compiler works for you: adding a variant makes every `match` that lacks an arm for it fail to build, and that error list is your to-do list. Prefer explicit arms for enums that are likely to grow, and keep `_` for values such as integers where listing every case is impossible.

The method: when a new case behaves like an old one, search for the places that decide per case and look for catch-all arms.
