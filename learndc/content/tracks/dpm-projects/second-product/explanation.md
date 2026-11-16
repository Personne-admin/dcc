A product is the unit dpm builds, and each product has its own entry module.
That is why two programs can live in one project: they are two entry files over
one set of modules. `harbor::ships` is compiled into each product that imports
it, but you maintain it once.

The `entry` field matters because the default, src/main.dc, can belong to only
one product. `dpm build` builds every product, so a typo in tonnage.dc fails the
whole build even if you only wanted to run one of them. `dpm run tonnage`
builds and starts just the product you name.
