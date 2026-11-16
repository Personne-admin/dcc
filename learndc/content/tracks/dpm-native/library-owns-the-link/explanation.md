Native requirements follow the code. `mathx::native` declares `sqrt` and `cos`,
so `mathx` is the package that needs libm, and dpm passes `-lm` for every
build that includes the package, however deep in the dependency graph it
is. The harbor program stays ignorant, which is exactly what makes it possible
to replace the wrapper with a pure DC implementation one day without touching any
consumer.

dpm also records the change: the lock's manifest fingerprint covers link
requirements, and changing a dependency's link line invalidates the build stamp,
so the product is relinked, not just recompiled.
