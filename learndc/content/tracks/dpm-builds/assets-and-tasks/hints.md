## Hint 1

An asset is `{ name = "ships", location = "path:data" }` in an `assets` list; a task refers to its directory as `${asset:ships}`. The task fields are lists, except name and description.

## Hint 2

`command = { "sh", "-c", "wc -l < \"$$1\" > \"$$2\"; cat \"$$2\"", "sh", "${asset:ships}/ships.txt", "${task_dir}/count.txt" }`. The remaining fields repeat the same two paths.

## Hint 3

Add `assets = { { name = "ships", location = "path:data" } },` and the task with `tools = { "sh", "wc" }`, `inputs = { "${asset:ships}/ships.txt" }` and `outputs = { "${task_dir}/count.txt" }`.
