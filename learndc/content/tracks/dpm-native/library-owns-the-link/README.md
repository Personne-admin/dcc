# A library that carries its own link

*Continues the harbor project: the mathx library wraps what link-libm called directly.*

`libs/mathx` is a small library package that wraps two libm functions behind
DC functions: `mathx::native::root` and `mathx::native::cosine`. The harbor
program depends on it and calls only those wrappers, so it should not know
that libm exists.

The build fails with `undefined symbol: sqrt`: nothing tells the linker about
libm. The mistake is in the wrong place if you fix it in the harbor
manifest, because every other program using `mathx` would have to repeat it.

## Your task

Make the project build by declaring the native requirement where it belongs:
in the manifest of the package whose code calls `sqrt` and `cos`. Leave the
harbor `project.dcon` and all sources as they are.

Check the result with `dpm build -v`: the dcc line has `-lm` although the
harbor manifest has no `link`.

The program prints:

```text
root of 2 = 1.4142
cosine of 0 = 1.0000
```

## Acceptance criteria

- `dpm build` succeeds and the program prints the two lines above.
- `project.dcon` of harbor has no `link` field.
