## Hint 1

An enum lists its variants separated by commas; payload-less variants are
just names. A function returning `Status(ConfigError)` fails by returning a
variant, e.g. `return ConfigError::MissingName;`, and succeeds with
`Status::Ok`.

## Hint 2

Add the two new variants under the `Print` variant. In each validator, test
the failing condition first and return the matching variant, then end with
`return Status::Ok;`. For the name, the length of a `[]const char` is
`name.len`. The port is valid only when it is at least 1 and at most 65535,
so it is invalid when it is below 1 or above 65535.

## Hint 3

In `src/config.dc`, add `MissingName,` and `BadPort,` to the enum. In
`validate_name`, `if name.len == 0 { return ConfigError::MissingName; }`.
In `validate_port`, `if port < 1 || port > 65535 { return
ConfigError::BadPort; }`. Keep `return Status::Ok;` as the last line of each.
