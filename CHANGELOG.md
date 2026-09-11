# Changelog

All notable changes to dcc are documented here. The format follows
Keep a Changelog 1.1.0.

## [Unreleased] - planned as v0.3.0

### Fixed - em64t backend correctness sweep

- Width-correct narrow-integer ALU, shift, compare, division, modulo, and
  negation codegen, including switch lowering.
- Aggregate handling: phi aggregates only into lowered predecessors,
  exact-width stores of register-returned aggregates, per-block
  materialization of constants and globals, and aborts on unmaterializable
  members instead of silent miscompiles.
- Register allocation: physical-register reads respected during reuse,
  distinct spill-reload scratches, int/float conversion operand
  classification, and parallel-copy resolution.
- Scalar floating-point loads and stores from globals now encode
  RIP-relative relocations instead of trapping.
- Restricted-value casts normalize the operand to the restriction's
  underlying type before the membership check.

### Added - standard library and checking

- New std::os modules with Linux and Windows backends: dir, thread,
  pipe, process, random, and net.
- First runtime coverage for -fbounds-check and -frestricted-check
  failure paths, with em64t harness support.
- Cross-backend differential corpus (make differential): 65 executable
  programs compared byte-for-byte across em64t O0/O1/O2 and LLVM O0.

### Known limitations

- Programs built with -fbounds-check or -frestricted-check fail to
  link: the compiler references a synthetic assert routine that no
  runtime provides yet.
- Constant integer arithmetic is not range-checked against its destination
  type (u8 x = 200 + 100 is accepted).
- Restriction endpoints accept different expression forms at module scope
  versus body scope.
- Nominal aliases do not propagate restricted-type membership diagnostics.
- The language, ABI, and CLI surface are still evolving; treat this as a
  pre-release.
