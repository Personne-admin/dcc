# Hosted library basics

Enable the library with `-flibdcext` (the default for DPM host products).
The prelude is supplied to user modules, including imported helpers. Library
implementation files and compiler core modules do not receive it. A module
can opt out with `module package::low_level @no_prelude;`; `-fno-prelude`
disables automatic injection for a compilation without disabling library
linking or the hosted default allocator.

## Heap and arrays

Hosted library builds select `std::os::heap::Heap` as `DefaultAllocator` unless
`-fdefault-allocator=module::Policy` selects another policy. SDK sessions can
set `CompileOptions.libdcext`; enabling the prelude also opts into the hosted
default. Freestanding targets retain the unselected default and require an
explicit policy. Use consistent allocator settings for separately compiled
objects: allocator arguments are part of container types.

`Heap` is zero-sized and needs no initialization. The initial implementation
uses OS page allocations. It returns a slice of exactly the requested length,
accepts power-of-two alignments up to the platform page size, and reports
`AllocError::InvalidAlignment` or `OutOfMemory`. A zero-length allocation returns
an empty slice and can be freed. The policy itself owns no allocations; the
caller owns every returned block.

`resize_in_place` preserves the address and contents on success and leaves the
allocation unchanged on failure. The caller updates its slice length only after
success. A nonempty block cannot resize in place to zero; free it or use the
existing moving `std::alloc::reallocate` helper. Moving reallocation allocates
first, copies the common prefix and frees the old block only after success.

Arrays grow through the existing static allocator contract. Register cleanup
with `defer values.deinit()`. Growth can invalidate borrowed pointers and slices.
Copying an owning array does not transfer ownership, and deinit does not destroy
resources owned by its elements. These ownership rules are unchanged.

Custom default allocator modules must be leaf dependencies of the allocator
configuration. They do not receive automatic prelude imports, which would
create a configuration cycle; use explicit imports there. A module-level
`@no_prelude` makes that policy explicit when compiling it in other contexts.

The public Heap policy does not expose page ownership or require raw page API
calls. Page-backed allocation is intentionally simple and can waste memory for
small objects; its internals can be replaced without changing `Array(T)` or
requiring per-instance heap state.
