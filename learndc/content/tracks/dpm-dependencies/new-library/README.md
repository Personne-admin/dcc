# Create a library package

*Continues the harbor project: after units and report, you write the third library yourself.*

The harbor program needs distances between four ports. The distance logic is
something other tools will want too, so it becomes a library package named
`ports`, in `libs/ports`, with a single module `ports::lookup`.

`src/main.dc` is written and calls the library:

```dc
println("port 0 to port 3: {} nm", ports::lookup::distance(0, 3))?;
```

The ports sit on one coastline at these positions in nautical miles:

| Port | Position |
|---|---|
| 0 | 0 |
| 1 | 35 |
| 2 | 80 |
| 3 | 140 |

## Your task

1. Create `libs/ports/project.dcon` with `name = "ports"` and a version, and
   no products.
2. Create `libs/ports/src/ports/lookup.dc` declaring `module ports::lookup;`
   with two public functions: `i32 position(i32 port)` (the table above, 0
   for an unknown port) and `i32 distance(i32 a, i32 b)` (the absolute
   difference of the two positions).
3. Add the dependency to the harbor `project.dcon`.

The program prints:

```text
port 0 to port 3: 140 nm
port 2 to port 1: 45 nm
port 3 to port 3: 0 nm
```

## Acceptance criteria

- The library has no products and lives under `libs/ports`.
- `dpm build` succeeds and the program prints the three lines above.
