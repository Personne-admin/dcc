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

Two further block attributes exist alongside the operand clauses:

- `arch("x86_64")` (or `"x86"`) pins the block to one architecture and
  is accepted on every `asm` form, including module scope. A block whose
  architecture does not match the target is an error, never silently
  dropped.
- `alignstack` requests stack alignment for the emitted call. It is
  accepted on function-scope blocks and has no effect on operand meaning.

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
  `in al`, `in dx`), a register family (`in accumulator`, resolved from the
  operand type width), or compiler-selected (no register written).
- `in imm` — an immediate. The value must be a compile-time integer or
  boolean constant. Immediates must be inputs and consume no register.
- `in mem` — memory, written `slot = *ptr`. The operand contributes the
  address of `*ptr`; the template references the whole memory location.
- `in edx:eax` — a register pair (high:low). Pairs are split into two
  operands during lowering and recombined afterwards.
- `in sym` — a symbolic reference to a dc global or function
  (`inputs(addr in sym = counter)`). Inputs only; see Symbol references.
- `in flag <condition>` — a condition-code output (`output(ok in flag zero)`).
  Outputs only, and the target must be `bool`; see Flag outputs.
- `in any` — register or memory, at the backend's choice
  (`inputs(v in any = x)`). See Register-or-memory.

Explicit registers must be distinct across all operands; sharing one
register between operands is rejected (use a single `inout` operand
instead). Reserved registers (`rsp`, `rbp`, …) cannot be operands or
clobbers.

A memory operand with a `const`-qualified pointee cannot be `inout`.

### Register families

`in accumulator` (likewise `base`, `counter`, `data`, `source`,
`destination`) names a register *family* instead of a concrete register.
Sema resolves the family from the operand type width after the type is
known, per template instantiation:

| family        | 8   | 16 | 32  | 64  |
|---------------|-----|----|-----|-----|
| `accumulator` | al  | ax | eax | rax |
| `base`        | bl  | bx | ebx | rbx |
| `counter`     | cl  | cx | ecx | rcx |
| `data`        | dl  | dx | edx | rdx |
| `source`      | sil | si | esi | rsi |
| `destination` | dil | di | edi | rdi |

The width comes from the operand type: integers and untagged enums by size,
`bool` as 8-bit, pointers as their architectural size. Anything else is
rejected naming the family, the type, and its size, as is any size other
than 1/2/4/8. No conversion or extension is ever inserted: the resolved
register always matches the operand width exactly, as with explicit
registers.

Register pairs accept families per half: `in data:accumulator` on `u64`
resolves to `edx:eax`, with each half resolved from its split width.
`ah`/`bh`/`ch`/`dh` are not reachable through families, and there are no
families for `rsp`/`rbp` or `r8`-`r15` (those names keep their explicit
meaning).

Generic port I/O:

```dc
public void out(T)(u16 port, T value) {
  asm @[intel, inputs(p in dx = port, v in accumulator = value)] {
    "out %[p], %[v]"
  };
}
```

`out!u8` pins `v` to `al` and `out!u16` to `ax`: the same body instantiates
at every width.

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

## Width views

A bracket reference may carry a size view: `%[name:byte]`, `%[name:word]`,
`%[name:dword]`, `%[name:qword]`, and the same for positional indices
(`%[0:byte]`). Views are bracket-only: a bare `%0:byte` is the operand `%0`
followed by the literal text `:byte`.

```dc
asm @[intel, output(dst = y in eax), inputs(v in ebx = x)] { "mov %[dst:dword], %[v:word]" };
```

- Views are allowed only on register operands. A view on `mem`, `imm`,
  `sym`, `flag`, or `any` is rejected, as is a view on a register pair
  (reference each half with `%%` instead).
- The view must not be wider than the operand's declared width: narrowing
  (`dword` on `u64`) is fine, widening (`qword` on `u32`) is an error.
- A `byte` view needs a byte subregister on the selected target. Explicit
  registers are checked in semantic analysis (`esi:byte` passes on x86_64
  via `sil` and fails on x86, which has no byte alias for `esi`);
  compiler-selected registers prefer a byte-capable family, and exhausting
  those is its own diagnostic rather than a register-count error.
- Views cannot be combined with `%c`/`%P` modifiers.
- The `:` inside any other construct keeps its normal meaning: `"%[p]: nop"`
  is the operand followed by a label colon, and `"fs:[%[p]]"` is a segment
  prefix around the operand. Only the colon inside the brackets is a view.

## Escaping `%` and per-scope references

Function-scope and module-scope templates accept different subsets:

| spelling          | function scope | module scope |
|-------------------|-----------------|--------------|
| `%%`              | literal `%` (`%%rax` → `%rax` AT&T, `rax` Intel) | same rendering, same register check |
| `%N`              | operand by index | rejected (`takes no operands`) |
| `%[name]`         | operand by name | rejected (`takes no operands`) |
| `%[name:view]`    | sized view (`${N:b/w/k/q}`) | rejected (`takes no operands`) |
| `%c[…]` / `%P[…]` | bare-symbol/immediate print | rejected (`takes no operands`) |
| `%{name}`         | rejected (`requires module-scope`) | mangled-name substitution |
| `%=`              | unique stamp per expansion | rejected (`not allowed`) |

Notes:

- `%c`/`%P` are prefix modifiers on operand references only. `%c{…}` is not
  a form: at module scope it is a parse error, and at function scope a
  modifier on `%{…}` cannot arise.
- `%[N:view]` takes a positional index inside the brackets; a bare `%N`
  never carries a view.
- A single `%` that does not start a valid reference is an error, as is an
  unterminated `%[name` or `%{name`. A `%%name` whose name is not a known
  register is rejected.

## Explicit registers

`in rax` pins the operand to that register. The compiler moves inputs there
before the asm and captures outputs afterwards; `inout` does both through
the same register. Explicit registers Perform no implicit conversion: the
operand type width must match the register width (`u32` with `eax`, `u64`
with `rax`, `u8` with `al`). For a width-polymorphic spelling, use a
register family instead: `in accumulator` resolves to `al`/`ax`/`eax`/`rax`
from the operand type.

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

## Symbol references

`inputs(addr in sym = counter)` binds a dc global or function as a symbolic
operand. Only inputs; only globals (`ModuleGlobal`, `Static`, `Extern`) and
(non-generic) functions resolve — anything else is rejected naming the
placeholder. LLVM receives an `s` constraint, so the reference keeps the
symbol alive through optimization and LTO the same way any other input use
does. The reference renders bare in both dialects (`%[addr]`), with `%c[…]`
and `%P[…]` accepted as explicit spellings of the same bare rendering.

```dc
u64 base = asm @[intel, inputs(addr in sym = table)] { "lea rax, %[addr]" };
```

## Flag outputs

`output(ok in flag zero)` captures a condition code as a `bool`. Only
outputs, and only `bool` targets:

| condition | aliases | LLVM |
|-----------|---------|------|
| `zero` | `equal` | `={@ccz}` |
| `not_zero` | `not_equal` | `={@ccnz}` |
| `carry` | `below` | `={@ccc}` |
| `not_carry` | `above_equal` | `={@ccnc}` |
| `above` | | `={@cca}` |
| `below_equal` | | `={@ccbe}` |
| `sign` | | `={@ccs}` |
| `not_sign` | | `={@ccns}` |
| `overflow` | | `={@cco}` |
| `not_overflow` | | `={@ccno}` |
| `parity_even` | | `={@ccp}` |
| `parity_odd` | | `={@ccnp}` |
| `less` | | `={@ccl}` |
| `less_equal` | | `={@ccle}` |
| `greater` | | `={@ccg}` |
| `greater_equal` | | `={@ccge}` |

```dc
bool same = false;
asm @[intel, output(same in flag equal), inputs(a in rax = x, b in rbx = y)] { "cmp %[a], %[b]" };
```

LLVM produces an `i8` that the backend truncates to dc `bool` (`i1`) at the
call site before storing, so upper bits can never leak through. A flag
output combined with `clobbers(cc)` is rejected: the output already models
the flags it reads. Unknown condition names are rejected listing nothing
further — the table above is the complete set.

## Register-or-memory

`in any` on an input or an output lets the backend choose a register or a
memory location per call site:

```dc
u64 y = asm @[intel, output(dst = y in any), inputs(v in any = x)] { "mov %[dst], %[v]" };
```

LLVM receives `rm` for inputs and `=&rm` for outputs. The `&` is load-bearing:
outputs are always early-clobber, exactly as with registers, so an output
never shares storage with an input.

The template must assemble correctly under either choice, and dcc cannot
check this. Safe:

```dc
asm @[intel, output(dst = y in any), inputs(v in any = x)] { "mov %[dst], %[v]" };
```

Unsafe — assumes `%[p]` is an address register, which breaks the moment LLVM
picks memory:

```dc
asm @[intel, output(dst = y in any), inputs(p in any = ptr)] { "mov %[dst], [%[p] + 8]" };
```

Rules: `inout … in any` is rejected (there is no tied `rm` spelling; a split
would let the read and the write pick different locations). Width views on
`any` are rejected. `any` accepts integer, pointer, `bool`, and untagged-
enum operands of at most 8 bytes; `float` is rejected. That restriction is
about the `rm` constraint spelling, not a fundamental limit, and may be
revisited.

## Labels

Two label forms work inside function-scope templates:

```dc
asm @[intel] { "jmp L%=\nL%=: nop" };
asm @[intel] { "1: pause\n jne 1b" };
```

- `%=` is a unique stamp. LLVM lowers it to `${:uid}`, which expands once
  per template instantiation at asm-print time — after inlining, unrolling,
  and jump threading — so duplicated blocks never emit duplicate symbols.
  Never mint the stamp earlier: any value fixed at emission time repeats in
every copy, which is exactly the failure `%=` prevents.
- Numeric local labels (`1:`, `1f`, `1b`) are assembler constructs. The
  template is opaque on the LLVM path, so they pass through untouched and
  the assembler's own local-label mechanism handles duplication. No
  validation is performed on them, deliberately: the assembler owns them.
- A named label (`Lfoo:`) breaks under inlining or repeated instantiation
  of the same template — every expansion emits the same symbol. Prefer `%=`
  or numeric labels for any block that can be duplicated.
- Jumping out of the block is undefined behavior: there is no `asm goto`,
  and the compiler models the block as falling through to the next
  statement.

## Module-scope asm

An `asm` block may appear at file scope, outside any function:

```dc
module test;

u64 counter = 42;

asm { ".quad %{counter}" };
asm @[intel] { "call %{handler}" };
```

- The block takes no operands and sees no locals. The only attributes are
  the dialect (`intel`/`att`) and `arch(…)`; `output`, `inputs`, `inout`,
  `clobbers`, `volatile`, and `alignstack` are rejected on module asm.
- `%{name}` (also `%{mod::name}`, resolved by its last segment) substitutes
  the mangled name of a same-TU global or non-generic function as bare
  text. Anything else in `%`-position follows the per-scope table: `%[…]`
  and `%N` are rejected, `%=` is rejected, `%c`/`%P` have no meaning here.
- Referenced globals and defined functions are appended to
  `llvm.compiler.used`, so an otherwise-unused symbol referenced only from
  module asm survives optimization and LTO. (Same-TU only is a deferral,
  not a rule: an imported/`extern` symbol needs only correct mangling — the
  assembler emits a relocation and no keep-alive is involved.)
- Blocks emit in source order relative to each other, at the top of the
  object regardless of where they sit among declarations. Position relative
  to functions carries no meaning.
- A non-matching `arch(…)` is an error, consistent with function-scope asm.
- `%%` renders a single `%` (AT&T) with the same register check as
  function scope; Intel `%%reg` renders bare for known registers.

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
any other pure code. Memory clobbers do not imply volatility: declaring
`memory` tells both backends the block reads and writes arbitrary memory,
but only `volatile` (the default) forces the block to execute and stay in
place.

`asm` is never a constant expression. CTFE and template-argument evaluation
treat an `asm` block as non-constant, so using one where a constant is
required (a global initializer, a non-type template argument) is an ordinary
"not a constant expression" diagnostic, never a crash.

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
per operand — `=&r` (or `={reg}`) for outputs, `r`/`{reg}`/`*m`/`i`/`s`
for inputs, ties for register `inout`, and `=*m` plus a second `*m` address
for memory `inout` (matching clang). Flag outputs become `={@cc<code>}`
returning `i8`, truncated to `bool` at the call site. Width views rewrite
to `${N:b}`/`${N:w}`/`${N:k}`/`${N:q}`. `rm`/`=&rm` cover `in any`.
`%=` becomes `${:uid}`; `%{name}` in module asm is substituted textually
before emission. Clobbers become `~{…}` entries. Multiple outputs return
an aggregate that lowering decomposes exactly as on native.

## Limitations

- Native code generation supports x86_64 only.
- The native backend implements a strict subset of inline assembly. Each
  of the following is diagnosed at its source range rather than diverging
  silently: symbolic (`in sym`) operands, module-scope `asm`, `%=` unique
  stamps, flag outputs, width views, and `in any` operands. Numeric labels
  and assembler directives are likewise rejected by the native instruction
  parser.
- Outputs never share registers with inputs (as if every output were an
  early clobber). Use `inout` for read/write values.
- 8/16-bit arithmetic is limited to moves, compares, and tests; use
  32/64-bit operations or extend explicitly.
- `inc`/`dec` support 64-bit registers only.
- Native `out`/`in`, `rdtsc`, atomics with `lock`, and control-flow
  instructions are rejected; several work through the LLVM backend.
  Register families resolve before either backend, so backend coverage is
  identical to explicit registers.
- The template is opaque on the LLVM path: undefined labels and
  cross-block label collisions are not diagnosed — the assembler owns
  them. Prefer `%=` or numeric labels over named labels, and never jump
  out of the block.
- `inout … in any` is rejected (no tied `rm` spelling exists).
- `float` is rejected for `in any`; the restriction is about the `rm`
  constraint spelling and may be revisited.
- Module-scope `%{…}` resolves same-TU globals and non-generic functions
  only (a deferral; imported/`extern` symbols need only mangling).

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

Flag capture after a compare:

```dc
bool same = false;
asm @[intel, output(same in flag equal), inputs(a in rax = x, b in rbx = y)] { "cmp %[a], %[b]" };
```

Backend-chosen placement:

```dc
u64 y = asm @[intel, output(dst = y in any), inputs(v in any = x)] { "mov %[dst], %[v]" };
```
