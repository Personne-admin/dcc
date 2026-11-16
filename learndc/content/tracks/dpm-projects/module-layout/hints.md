## Hint 1

The file path below `src` is the module path: `harbor::report` is `src/harbor/report.dc`, and its first line is `module harbor::report;`.

## Hint 2

Copy the Ship struct, FLEET and total_tons into harbor::ships and mark them public. print_line mentions `Ship` in its signature, so harbor::report needs `public import harbor::ships;`. Inside the same namespace the shared `harbor::` prefix is dropped, so the type is written `ships::Ship`.

## Hint 3

In report.dc: `module harbor::report;`, `public import harbor::ships;`, then `public Status(Error) print_line(ships::Ship ship) { println("{} {} t", ship.name, ship.tons)?; return Status::Ok; }`.
