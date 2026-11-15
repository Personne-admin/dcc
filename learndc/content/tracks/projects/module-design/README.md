# Design a module

`src/main.dc` holds a whole program: it works out what a parcel costs to ship
and prints a small price table. The pricing logic and the printing are mixed
in one file, and every function is visible to everything. You will give the
pricing its own module and decide what that module shows to the world.

```text
250 g: 350 / 525
1200 g: 500 / 750
5000 g: 650 / 975
20000 g: 800 / 1200
```

The three pricing functions in `main.dc` are:

- `weight_class(grams)`: a helper that turns a weight into a class 1 to 4.
- `surcharge(base)`: a helper that computes the express supplement.
- `shipping_cost(grams, express)`: the only function a caller cares about.

## Your task

1. Create `src/shipping.dc` that starts with `module shipping;`.
2. Move `weight_class`, `surcharge` and `shipping_cost` into it, unchanged.
3. Mark only `shipping_cost` as `public`. The two helpers stay private.
4. In `src/main.dc`, remove the moved functions, add `import shipping;` and
   call `shipping::shipping_cost(...)`.

The price table must print exactly as before. Do not rename anything: the
checks look for these names.

## How privacy is checked

The first harness calls `shipping::shipping_cost` like any caller. The second
one brings in the whole public surface with `using shipping::*;` and declares
its own `weight_class` and `surcharge`. If your helpers are private, the
checker's own functions are the only ones and everything is fine. If you
made a helper public, the call is ambiguous and the build fails: the helper
leaked out of the module.

## Acceptance criteria

- The project builds with `dpm build` and prints the table above.
- `shipping.dc` declares `module shipping;`, and only `shipping_cost` is public.
- `main.dc` contains no pricing logic any more.
