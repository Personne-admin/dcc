# Add a second product

*This lesson starts the harbor project. The next lessons of this chapter continue it.*

The harbor project lists its fleet with the product `fleet` (`src/fleet.dc`).
The harbormaster also wants a small program that prints only the total
tonnage. It belongs in the same project, because it shares the
`harbor::ships` module.

The project also has a third product, `audit`. Its entry, `src/main.dc`, is a
small program from the course that reads `project.dcon` and your source files
and prints a checklist. Leave `src/main.dc` and `src/audit.dc` alone; run
`dpm run audit` whenever you want to see what is still missing.

## Your task

1. Create `src/tonnage.dc` with `module tonnage;`. Its `main` prints the sum
   of the fleet's tons by calling `harbor::ships::total_tons()`.
2. Declare a product named `tonnage` in `project.dcon` whose entry is
   `src/tonnage.dc`.

`dpm run tonnage` must print:

```text
total tons: 445
```

`dpm build` builds every product, so a mistake in `tonnage.dc` also fails the
build of the others.

## Acceptance criteria

- `dpm run audit` prints eight `ok` lines and `8 checks, 0 failed`.
- `dpm run tonnage` prints the line above.
