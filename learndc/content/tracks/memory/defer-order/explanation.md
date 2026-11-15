# Why the order comes out right

`main` registers two deferred statements and the inner block one. The block
ends first, so `plates cleared` appears before `party running`. When `main`
reaches `return`, its deferred statements run last-in, first-out: the music
was switched on after the lights, so it goes off first.

Writing each `defer` next to its partner keeps the pairs visible and makes
the teardown order follow automatically from the setup order. If someone
later adds `println("fog machine on")?;` with its own `defer`, it is switched
off first without touching any other line.
