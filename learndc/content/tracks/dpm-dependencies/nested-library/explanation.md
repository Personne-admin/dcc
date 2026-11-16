Every package declares what it imports itself, and dpm merges the declarations
into one build list. The harbor manifest says what the harbor program needs;
`libs/report/project.dcon` says what report needs. That keeps a library's
requirements next to the code that has them: when report stops using units, only
its manifest changes, and nobody downstream has to be told.

`dpm tree` shows the merged graph and `dpm why path:libs/units` the chain that
required it. The lock lists both packages, with `units` recorded under report's
dependencies. Importing a module from a transitive package works, but it is a
habit to avoid: if report drops units, your program breaks. Declare what you
import yourself.
