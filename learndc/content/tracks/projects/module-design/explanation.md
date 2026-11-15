# Why this design works

`shipping` shows one function. A caller needs to know how much a parcel costs;
how the weight classes are cut or how the express supplement is computed is
the module's own business. Because `weight_class` and `surcharge` are
private, you can change the class limits, rename the helpers or replace them
with a table, and no file outside `shipping.dc` can break.

`main` is now only about presentation. It imports the module, calls
`shipping::shipping_cost` with the qualified name and prints.

The privacy check works because a public name brought in by `using
shipping::*;` competes with a local declaration of the same name, and a call
that matches both is ambiguous. Private names are never brought in, so the
local ones win unopposed. In your own code the same effect is a good smell
test: when a module's public list reads like its whole source file, the
module has no surface.
