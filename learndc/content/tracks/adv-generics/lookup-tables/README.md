# Tables built by the compiler

`src/tables.dc` declares two value aliases that the compiler fills:

```dc
using u8[128] CLASSES = build_classes();
using u8[128] ROT13 = build_rot13();
```

The functions `class_of` and `rot13` already read them with a run-time
index. Your task is to write the two builders, which currently return an
array of zeros:

- `build_classes` returns, for each ASCII code 0 to 127, one of `DIGIT`
  (`0` to `9`), `UPPER` (`A` to `Z`), `LOWER` (`a` to `z`), `SPACE` (space,
  tab 9, newline 10) or `OTHER`.
- `build_rot13` returns, for each code, the character after rotating a letter
  13 places inside its alphabet (`a` becomes `n`, `z` becomes `m`, `N` becomes
  `A`). Every other code maps to itself.

A builder is a normal function: a `u8[128]` local, a `for` loop over `usize`
indexes, assignments into the table, `return table;`. It may use the named
constants above. It must not read global variables or print, or the alias is
no longer a constant.

Character codes: `'0'` is 48, `'A'` is 65, `'a'` is 97. Convert a computed
number to a byte with `as u8`.

`main.dc` counts the classes of a sentence and prints it through `rot13`:

```text
digits 4 upper 4 lower 13 spaces 4 others 2
Uryyb Jbeyq 2026: QP ebpxf!
```

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints the two lines above and exits with 0.
- The harness checks every ASCII code against the rules.
