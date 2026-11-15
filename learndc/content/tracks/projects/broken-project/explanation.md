# Why the solution works

The project had three independent defects, each reported by a different part
of the toolchain.

1. `project.dcon:5:42: entry file 'src/app.dc' does not exist`. `dpm` checks
   the manifest before it starts the compiler. The entry path is relative to
   the project directory and the real file is `src/main.dc`.
2. `module declaration stat does not match resolved path stats`. When `main`
   writes `import stats;` the compiler looks for `stats.dc` and expects it to
   declare `module stats;`. The follow-up errors (`could not resolve module`
   and `unknown path`) are consequences: once the module fails to load, every
   name from it is unknown too. When a diagnostic list looks long, the first
   entry is usually the cause.
3. `unknown path stats::average`. This time the module loaded, but only
   `largest` was `public`. Declarations are private by default, so `average`
   does not exist from `main`'s point of view.

The calls in `main.dc` never needed to change; the tools told us where each
mismatch was.
