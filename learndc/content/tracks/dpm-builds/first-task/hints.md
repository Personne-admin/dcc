## Hint 1

Tasks are a project-level list next to `products`. A task is a struct; the fields you need are `name`, `description`, `command` and `build`. Both `command` and `build` are lists.

## Hint 2

`${out:fleet}` is the token for the output file of the product `fleet`. The command is the single element list `{ "${out:fleet}" }`, and `build = { "fleet" }` makes dpm build the product first.

## Hint 3

`tasks = { { name = "fleet-report", description = "Run the fleet program", command = { "${out:fleet}" }, build = { "fleet" } } },`
