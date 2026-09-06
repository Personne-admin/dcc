# Inline assembly

dcc supports inline assembly through `asm` statements and `asm` expressions.
One frontend (parser, sema, IR lowering) serves both backends, so operand
meaning, `%0` / `%[name]` resolution, operand directions, and clobbers behave
identically whether the program is compiled with the native em64t backend or
the LLVM backend. Only code emission differs.

## Syntax

An `asm` block takes a template string followed by operand clauses in
`@[...]` attributes:

```dc
asm @[intel, output(dst = y in rax), inputs(v in rbx = x)] { "mov %[dst], %[v]" };
```

As an expression, `asm` evaluates to its first output:

```dc
u64 y = asm @[output(u64 in rax), inputs(v in rbx = x)] { "mov %[v], %0" };
```

When an `asm` statement declares no `output` and no `inout`, and the result
is assigned to a variable, the compiler synthesizes one output bound to that
variable:

```dc
u64 y asm @[inputs(v in rbx = x)] { "mov %[v], %[y]" };
```

## Operand directions

Each operand is an input, an output, or both:

```dc
asm @[
    output(dst = y in rax),
    inputs(v in rbx = x),
    inout(acc = total in rcx),
] { "..." };
```

- `output(name = place)` writes `place` after the asm runs. `place` must be
  a writable lvalue.
- `inputs(name = value)` evaluates `value` before the asm runs.
- `inout(name = place)` reads `place` first, then writes it back. The same
  physical register (LLVM: a tied constraint) carries the value both ways.

An output may carry a type instead of a destination:

```dc
u64 y = asm @[output(u64 in rax)] { "..." };
```

Without a type or a destination, the type is deduced from the context (asm
expressions) or from the bound variable.

## Placement kinds

After an optional `in`, each operand selects a placement:

- `in reg` — a general register. Either an explicit register (`in rax`,
  `in al`, `in dx`) or compiler-selected (no register written).
- `in imm` — an immediate. The value must be a compile-time integer or
  boolean constant. Immediates must be inputs and consume no register.
- `in mem` — memory, written `slot = *ptr`. The operand contributes the
  address of `*ptr`; the template references the whole memory location.
- `in edx:eax` — a register pair (high:low). Pairs are split into two
  operands during lowering and recombined afterwards.

Explicit registers must be distinct across all operands; sharing one
register between operands is rejected (use a single `inout` operand
instead). Reserved registers (`rsp`, `rbp`, …) cannot be operands or
clobbers.

A memory operand with a `const`-qualified pointee cannot be `inout`.

## Positional operands

`%0`, `%1`, … refer to operands by declaration order, starting at zero.
The index counts every declared operand, including outputs.

```dc
u64 y = asm @[output(u64 in rax), inputs(v in rbx = x)] { "mov %1, %0" };
```

Out-of-range indices are rejected. Operands that are register pairs cannot
be referenced by a single `%N`; reference would be ambiguous, so it is an
error.

## Named operands

Any operand with a `name =` prefix can be referenced as `%[name]`:

```dc
asm @[intel, output(dst = y in rax), inputs(v in rbx = x)] { "mov %[dst], %[v]" };
```

Names must be unique within one `asm`; unknown names are rejected. The same
operand may be referenced any number of times, and positional and named
references may be mixed freely (`%1` and `%[v]` resolve identically).

## Escaping `%`

`%%` escapes a literal percent sign:

- In AT&T templates, `%%rax` addresses the literal register `rax`
  (`"mov $42, %%eax"`).
- In Intel templates, `%%rax` also works and renders as bare `rax`.
- A bare `%%` renders a single `%`.

A single `%` that does not start a valid reference is an error, as is an
unterminated `%[name`. A `%%name` whose name is not a known register is
rejected.

## Explicit registers

`in rax` pins the operand to that register. The compiler moves inputs there
before the asm and captures outputs afterwards; `inout` does both through
the same register. Explicit registers Perform no implicit conversion: the
operand type width must match the register width (`u32` with `eax`, `u64`
with `rax`, `u8` with `al`).

## Immediates

`in imm` operands render as plain numbers (Intel) or `$`-prefixed numbers
(AT&T). Example:

```dc
u64 y = asm @[output(u64 in rax)] { "mov $42, %0" };
```

## Memory operands

`inout(slot = *p)` (or `inputs`/`output` with `*p`) passes the address of
`*p`. Inside the template, `%[slot]` denotes the whole memory location:

```dc
asm @[inputs(val = v), inout(slot = *p)] { "mov %[slot], %[val]" };
```

In Intel templates the substituted text carries the pointee width
(`qword [rbx]`), so plain references work. Brackets may also be written
explicitly around a reference to add displacement or indexing, in which
case the reference contributes just the address register:

```dc
asm @[intel, output(u64* in rax), inputs(ptr = p)] { "lea %0, [%[ptr] + 8]" };
```

AT&T composition works the same way: `4(%[p])`, `(%[p], %[q], 4)`.

## Clobbers

```dc
asm @[inputs(v = x), clobbers(rcx, memory, cc)] { "..." };
```

- Registers (`clobbers(rcx)`) may not overlap operands or reserved
  registers. The native backend spills live values around the asm; LLVM
  receives `~{rcx}`.
- `memory` models arbitrary memory reads/writes (`~{memory}` on LLVM).
  Memory-touching asm that the compiler must not reorder or delete should
  also be `volatile` (the default).
- `cc` models condition-code clobbers (`~{cc}` on LLVM). Flags are never
  live across inline asm in either backend, but declaring `cc` documents
  intent and keeps LLVM code generation conservative.

Neither backend invents clobbers: if the template touches a register or
memory, declare it.

## Dialects

`@[intel]` selects Intel syntax (`mov dst, src`, `[base + index*scale +
disp]`, bare registers and immediates). The default is AT&T
(`mov src, dst`, `disp(base, index, scale)`, `%reg`, `$imm`, mnemonic size
suffixes such as `movq`). LLVM receives the matching dialect flag, and the
native backend parses the matching grammar into the same instruction
representation.

The native backend accepts a documented subset (below) in both dialects.
AT&T size suffixes select the operand width; without a suffix the width is
inferred exactly as in Intel mode.

## Volatile behavior

`asm` blocks are volatile by default: they always execute and never move
across observable behavior. `@[volatile(false)]` lets the compiler treat
the block as a pure value computation, which may then be optimized like
any other pure code. Memory clobbers do not imply volatility.

## Native instruction subset

The native backend parses each template line into dcc's own em64t MIR and
encodes it with the shared x86 encoder — no external assembler is used.
Supported mnemonics:

- `mov` (8/16/32/64-bit `r, r/i/m` and `m, r/i`), `movzx`/`movsx`,
  `movq` (GPR/XMM), `movsd`/`movss` (XMM/XMM), `xchg` (64-bit), `lea`
  (64-bit), `nop`.
- `add`, `sub` (32/64-bit `r, r/i/m`); `and`, `or`, `xor` (32/64-bit
  `r, r/i/m`, plus 64-bit `m, r`); `imul` (32/64-bit `r, r | r, r, i |
  r, m`); `neg`, `not` (32/64-bit); `inc`, `dec` (64-bit).
- `cmp` (8/32/64-bit `r, r/i`, 32/64-bit `r, m`); `test` (`r, r` in
  8/32/64 bits, `r, i` in 64 bits).
- `shl`, `shr`, `sar` (32/64-bit `r, cl/i`); `push`, `pop` (64-bit).

Memory operands support `[base]`, `[base ± disp]`, `[base + index]`,
`[base + index*scale]`, and `[base + index*scale ± disp]` with scales 1,
2, 4, 8 and an optional `byte`/`word`/`dword`/`qword` size prefix.
Immediates accept decimal and `0x`/`0b`/`0o` forms; each instruction
validates its range.

Anything else produces a compiler diagnostic naming the problem:
unknown mnemonics, wrong operand counts or kinds, width mismatches,
ambiguous memory widths, bad scales or displacements, the `lock`/`rep`
prefixes, calls, jumps, returns, privileged instructions, `rdtsc`,
`cpuid`, assembler directives, and symbol or label references (pass
addresses as operands instead).

## LLVM mapping

The LLVM backend derives everything from the same operand model: the
template is rewritten to `$N` numbering (outputs first, then inputs),
immediate `$5` spellings become bare `5`, and constraints are generated
per operand — `=&r` (or `={reg}`) for outputs, `r`/`{reg}`/`*m`/`i` for
inputs, ties for register `inout`, and `=*m` plus a second `*m` address
for memory `inout` (matching clang). Clobbers become `~{…}` entries.
Multiple outputs return an aggregate that lowering decomposes exactly as
on native.

## Limitations

- Native code generation supports x86_64 only.
- Outputs never share registers with inputs (as if every output were an
  early clobber). Use `inout` for read/write values.
- 8/16-bit arithmetic is limited to moves, compares, and tests; use
  32/64-bit operations or extend explicitly.
- `inc`/`dec` support 64-bit registers only.
- Native `out`/`in`, `rdtsc`, atomics with `lock`, and control-flow
  instructions are rejected; several work through the LLVM backend.
- Inline asm cannot define labels, symbols, or relocations.

## Examples

Move with named operands (Intel):

```dc
u64 y = 0;
asm @[intel, output(dst = y in rax), inputs(v in rbx = x)] { "mov %[dst], %[v]" };
```

In-place increment (either dialect):

```dc
asm @[inout(acc = total in rax)] { "inc %[acc]" };
```

Memory store through a memory operand:

```dc
asm @[intel, inputs(val = v), inout(slot = *p)] { "mov %[slot], %[val]" };
```

Address computation with an explicit displacement:

```dc
u64* q = asm @[intel, output(u64* in rax), inputs(ptr = p)] { "lea %0, [%[ptr] + 8]" };
```
