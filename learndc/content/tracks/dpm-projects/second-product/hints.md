## Hint 1

A product is one entry in the `products` list of `project.dcon`. It needs a `name`, and an `entry` when the file is not src/main.dc.

## Hint 2

Add `{ name = "tonnage", entry = "src/tonnage.dc" },` below the other products. In the new file import `harbor::ships` and call `total_tons()` with the module name in front.

## Hint 3

src/tonnage.dc has `module tonnage;`, `import harbor::ships;` and `public Status(Error) main()` containing `println("total tons: {}", harbor::ships::total_tons())?;` and `return Status::Ok;`.
