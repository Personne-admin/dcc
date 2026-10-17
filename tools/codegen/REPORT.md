# Shared register allocation, part 1 (2026-10-17)

This step changes allocation in `compiler/source/backend/x86/regalloc.cppm` for both custom backends, at O0 and O2. It does not change shared IR, calling conventions, fixed-register policies, or LLVM. Complete measurement rows, commands, before/after MIR and listings, seeded execution results and byte attribution are retained in `~/.cache/dcc-regalloc-batch/`. Core5 was compiled read-only; nothing was pushed.

## Allocation changes and boundaries

Spill cost counts definitions once and uses twice, multiplies by 10 per natural-loop nesting level (capped at four), and weights unreachable-ending blocks and their exclusively terminating predecessors by 0.001. A logarithmic interval-length penalty balances frequent uses against long occupancy. Dominators identify natural backedges; nested loops contribute separate levels. The cheapest eligible interval is spilled, with end position breaking equal-cost ties. Constants and static address recipes receive a lower cost because they need no stack traffic.

Liveness now retains separate block segments. Previously, an interval reserved a register through every block between its first definition and final use, including unrelated cold assertion blocks. Call crossings, fixed-register clobbers and interference now consult the actual segments. This matters both for the guiding example and for safely recognizing non-interfering copies. A value still receives one register for its entire lifetime: this is lifetime-hole handling, **not live-range splitting**.

Single-definition integer immediates and LEA frame/global addresses with no register dependencies can be rematerialized when selected for spilling. Their original definition disappears, no spill slot is allocated, and the same flag-preserving recipe is emitted into the constrained scratch register at each use. Mutable memory loads, computed addresses, multiple definitions/phi destinations and instructions with implicit register dependencies are excluded.

Copy affinities prefer the already assigned source/destination register for non-interfering virtual ranges and physical incoming/outgoing copies. Allocation still checks every fixed-register clobber, incoming physical read, call-preserved restriction, shift destination, byte-register subset and 16-bit address-register subset. Phi copies use the existing parallel-copy resolver; mixed-width cycle preservation is unchanged. Redundant full-width and i8086 word self-copies disappear, while x86-64 dword self-copies that clear upper bits remain.

After parallel-copy resolution, callee-save insertion and physical legalization, block-local tracking removes a repeated spill reload only when the same physical register still contains the same slot with the same load opcode. Explicit/implicit register definitions and writes to that slot invalidate the record. Slots are compiler-private, nonescaping spill storage. No reuse crosses a block boundary. DI remains the permanently reserved i8086 scratch register.

Focused MIR units cover rematerialized constants/frame/global addresses, incoming/dying copy elimination, repeated reloads, a hot loop value under pressure, and a cold terminal call beside a hot use. The width test now loads runtime values to force genuine spills; constants no longer do so.

The full gate exposed a pre-existing test defect, fixed separately in `986270a fix: correct clobber fixture intel operand order`. The Intel-syntax template in `asm_multi_exec` used `mov input, output`, leaving the declared output unwritten; the old allocation accidentally retained the input in that output register. It now uses `mov output, input`. The corrected entire fixture passes LLVM/custom O0/O2 under both the original and new compiler (eight executions). The allocator did not miscompile a correctly constrained asm operation. Existing spill-only units now use non-rematerializable runtime loads, and three listing fixtures retain their width/segment/offset assertions with the new chosen registers.

## Measurements

The same 71-program corpus runs in all five O2 configurations, with whole-program compilation for imported programs: 355 measurements and 142 native executions in each run, all passed. The deterministic QEMU instruction plugin and calibration/accounting checks are unchanged. Counts are instructions rather than cycles; static totals include repeated rooted assert/panic implementations in separately compiled programs.

| Configuration | Code bytes | Executed instructions | Spill slots | Summed frame bytes |
| --- | ---: | ---: | ---: | ---: |
| small | 73,620 → 62,693 (-14.8%) | 168,921 → 130,392 (-22.8%) | 371 → 257 | 1,916 → 1,684 |
| unreal | 73,614 → 62,681 (-14.9%) | 168,885 → 130,346 (-22.8%) | 371 → 257 | 1,916 → 1,684 |
| unreal32 | 79,144 → 68,246 (-13.8%) | 166,498 → 129,155 (-22.4%) | 359 → 256 | 1,950 → 1,726 |
| custom em64t | 78,259 → 73,863 (-5.6%) | 92,191 → 87,074 (-5.6%) | 22 → 1 | 11,112 → 10,936 |
| LLVM x86_64 | 36,209 → 36,209 | 26,775 → 26,775 | 343 → 343 | 10,584 → 10,584 |

Small executes 54,256 → 14,410 spill stores/reloads (-73.4%). Its register copies rise 24,820 → 26,126: retaining values and resolving their physical constraints sometimes substitutes copies for much more expensive spill sequences. Em64t executes 1,748 → 72 spill stores/reloads and 11,813 → 8,396 register copies. Em64t is now **2.04× LLVM code bytes and 3.25× executed instructions**, compared with 2.16×/3.44× before. LLVM output/metrics are unchanged.

| Representative function | Bytes before → after | Executed before → after | Spills before → after | Frame before → after |
| --- | ---: | ---: | ---: | ---: |
| small/unreal unsigned i64 helper, i64 matrix | 1,410 → 1,107 | 40,494 → 31,748 | 30 → 16 | 154 → 106 |
| small/unreal signed i64 helper, i64 matrix | 724 → 656 | 1,996 → 1,842 | 15 → 11 | 84 → 70 |
| unreal32 signed i64 helper, i64 matrix | 756 → 675 | 2,020 → 1,838 | 15 → 10 | 86 → 68 |
| small insertion sort | 212 → 174 | 18,870 → 13,789 | 4 → 3 | 8 → 8 |
| em64t insertion sort | 231 → 222 | 10,003 → 9,505 | 0 → 0 | 16 → 16 |

All measured regressions remain included. Small `calls-internal-exec` main grows 580 → 589 bytes, 158 → 161 instructions and frame 8 → 32 as whole-range choices exchange reloads for stores around calls. Its pressure function grows three bytes/one copy. The small realistic `divide` wrapper grows 165 → 178 bytes and 56 → 60 instructions: an i64 argument occupies SI, while rematerializing its constrained frame address through DI needs AX preservation around loads. These are concrete reasons to pursue splitting and a cost model that accounts for constrained rematerialization, rather than assuming every spill reduction wins locally. No em64t function grows in this corpus. Full changes: `measurement-diff.json`.

## Core5 stage 2

Commands retain the requested Main.dc/custom/i8086-binary/unreal/O2/whole-program/loader_entry/include flags for `-S`; `-c` supplies symbol sizes and `-fdump-mir` supplies allocation evidence. Main source SHA256 remains `d60c82d8a6df94836fcd4da846d79a0093fa7874ee0542b14b25547eba09a3ec`. No Core5 file was modified and the program was not executed.

| Function | Code bytes before → after |
| --- | ---: |
| character/color render | **370 → 290** |
| __assert | 196 → 188 |
| slice/color render | 121 → 94 |
| slice render | 114 → 87 |
| loader_entry | 66 → 66 |
| Total executable function code | **867 → 725 (-16.4%)** |

Character render's frame shrinks 8 → 2 bytes and spills 4 → 1. `self` stays in SI, its eleven former reloads through DI disappear, and ordinary field accesses directly use SI. The remaining two-byte spill holds color; the one load from the same numeric frame offset is consequently not a self reload. The requested current Main.dc produces no calls inside this character overload, despite the original report mentioning a cold __assert call; its starting size does match 370 bytes. Its self spill is explained by the old farthest-end policy and coarse interference, rather than a call in this emitted function. Cold-call interference is independently covered by the focused MIR regression. Before/after listings, MIR, objects and symbol tables are in `real-before/` and `real-after/`.

## Next opportunities and live-range splitting

These are overlapping investigation ranges, not additive promises or implemented changes.

| Rank | Opportunity and evidence | Location | Estimated further code / executed gain | em64t | Risk |
| --- | --- | --- | --- | --- | --- |
| 1 | Fold immediates/addresses and remove backend ABI-boundary materialization. Slice headers still get forced stack slots; zero-GEP arithmetic and register-immediate-copy sequences remain. Copies are now 20.0% of small executed instructions. | mode-specific isel, shared MIR cleanup | 3–8% / 3–10% | yes | medium: widths, segmented addressing and carry chains |
| 2 | CSE/GVN, LICM and alias-aware ordinary load/store elimination. Sort/hash still repeat address and bounds work that LLVM folds or hoists. | shared IR analyses/transforms | 2–5% / 3–10% | yes | medium/high: effects, aliases and increased pressure |
| 3 | Live-range splitting and constrained-rematerialization cost, then reconsider permanent DI reservation. One assigned register or whole-range spill remains; small still executes 14,410 spill operations (11.1%), mainly wide helpers. Call-heavy main's frame regression identifies the need to spill only near calls and reload outside hot loops; dividing SI occupancy between wide arguments and address uses could fix the wrapper regression. | shared allocator; i8086 policy; pair-aware isel | 1–4% / 2–6% overall; greater in selected wide helpers | modest corpus-wide: only 72 executed spill operations remain, but calls/copies can benefit | high: split edges, pair halves, scratch constraints and ABI saves |
| 4 | Switch-result tables, loop strength reduction and later em64t vectorization. Small switch main still executes 33,767 instructions; em64t 45,177 versus LLVM's 13,937. | shared loop/CFG work, target switch isel | 1–5% / 3–12%; unrolling can grow code | yes; SIMD only on suitable targets | medium/high: wrapping induction, trip counts and growth |
| 5 | Branch layout/inversion and redundant jump elimination. Conditional-plus-unconditional sequences remain, including in render. | MIR layout/peephole | 1–3% / 1–5% | yes | low/medium: flags, fallthrough and relaxation |

Lifetime holes solve the false interference exposed by render, but do not let a value change register, live in memory only across a call, rematerialize selectively on one edge, or share a spill slot with a non-interfering spilled value. Those remain for a separate approved step. Fastestcall and strict -farch 8086 remain later roadmap work.

## Verification and baseline

The six fixed seeds plus seeds 4000–4299 pass: **306 seeds / 2,754 executions**, zero mismatches across LLVM O0, custom em64t O0/O2 and all three i8086 models O0/O2. Targeted i8086 differential/QEMU matrices pass **546 executions and 150 byte-identical object/listing linked images**. Both complete measurement runs and deterministic instruction accounting/calibration pass. The corrected asm fixture adds eight successful old/new-compiler LLVM/custom O0/O2 executions.

The end-of-batch full gate ran **once**, with LLVM enabled and zero skips. It passed 33 suites and exposed three suites with stale expectations: the reversed Intel fixture, register-specific listing expectations and spill units whose constants now rematerialize. After correcting those tests, only the affected suites were rebuilt/rerun: em64t **607/607**, real16 **896/896**, dispatcher **2,220/2,220**, all passed. The other 33 suites were not repeated; shared backend units passed **931/931** in the original gate. No compiler change was made after the measured/platform-tested version.

Linux passed in **243 s**: 104 OS checks, 176 layout assertions, 25 SysV ABI pairs and ELF/COFF O0/O2 unwind checks. Windows passed in **523 s**: 105 OS checks, 371 layout assertions, 25 Win64 ABI pairs and both Windows compiler backends. Snapshot `0afefb8ac0ff359a9eae5fbfd739a6b9a60d2e39` has the final compiler sources; later differences are regression-test corrections/additions and this report. The measured compiler SHA256 is `1f317b253acd1aae07446a47e5c96eedc9f84322a628c1cd53ffe69c3f39205f`.

The preview against baseline-016131b attributes 3,573 objects and 3,573 matching listings to the allocator changes at O0/O2, with no added/removed records, status changes or immutable data-section changes. The separate asm-fixture correction additionally changes its eight object/listing records relative to that preview. Final independent-capture attribution and adoption are recorded in AGENTS.md and `final-baseline-adoption.json`; every changed artifact is named in `artifact-attribution.json` and `changed-artifacts.txt`. Native capture policy is unchanged; i8086 bytes remain covered by the measurements and QEMU/listing matrices. Previous baselines are preserved.

---

# O2 integer combining and SCCP (2026-10-15)

Two changes complete this approved step: `d9e723c fix: avoid clearing fully overwritten i8086 aggregate temporaries`, followed by an O2-only shared integer combining/SCCP pass. Evidence, exact commands, full per-function rows, MIR and disassembly: `~/.cache/dcc-combine-batch/`. No project source was modified and nothing was pushed.

## Division-helper regression investigation

The pre-SROA and post-SROA MIR are retained as `pre-sroa-divmod.mir`, `sroa-divmod.mir` and `divmod-mir.diff`. The signed helper's generated source is in `compiler/source/dcc.vfs.cppm` (`__dcc_divmod64`, around line 446); its two u64 result fields are conditionally negated. SROA changes the result from an in-place memory update into four live dword halves and phi-edge copies. That increases pressure across both sign branches and the eventual aggregate return.

There is also a local lowering defect: `compiler/source/backend/i8086/isel.cppm`, `memory_value` (around line 965), cleared every aggregate temporary before writing its fields. The reconstructed 16-byte DivResult has no padding and both runtime members overwrite every byte. Its unnecessary `REPSTOSW` clobbers AX/CX/DI, triggering additional saves/reloads while the four halves are live. Before SROA, MIR had three memory-copy operations and no reconstruction clear; afterwards it had `REPSTOSW`, one final copy and additional phi/spill transfers. MOV32 stores/reloads grew from 13/29 to 22/38, and COPYs from 6 to 14.

The local fix skips the clear only when every member has an explicit store rather than a known-zero initializer, the ranges are contiguous, and together they cover the entire object. Padded/partial/zero initializers retain the old clear. The regression unit checks both a complete pair and a padded aggregate; the existing far-pointer store explicitly clears its padding, and memory/QEMU matrices exercise those layouts.

| Signed helper | Bytes | Executed instructions in i64 matrix | Spill slots | Frame bytes |
| --- | ---: | ---: | ---: | ---: |
| pre-SROA small/unreal | 689 | 1,992 | 11 | 84 |
| SROA baseline small/unreal | 778 | 2,212 | 17 | 90 |
| local fix and final small/unreal | 724 | 1,996 | 15 | 84 |
| SROA baseline unreal32 | 770 | 2,080 | 15 | 86 |
| local fix and final unreal32 | 756 | 2,020 | 15 | 86 |

The fix recovers 216 of the 220 additional executed instructions on small/unreal. The remaining +35 bytes/+4 executed instructions versus pre-SROA are phi-copy/spill and aggregate return materialization costs, not a redundant-clear problem. They remain for the allocator/isel step: coalescing dword halves and phi edges, shortening live ranges, avoiding spill-to-spill copies, and improving result materialization. No register-allocator change was made here. The final combining pass does not change this helper further.

## Shared IR change and correctness boundaries

Two function passes, `combine` then `sccp`, run after mem2reg and before DCE/simplifycfg at **O2 only**. Combining handles integer identities, constant arithmetic, double negation/complement, redundant masks, extension/truncation chains and self-comparisons. Known bit masks justify removing redundant narrowing round trips; required result types remain intact. Floating-point identities are not inferred, so NaNs and signed zero are unaffected.

SCCP uses unknown/constant/overdefined values, sparse def-use worklists and executable predecessor edges. Phis meet only executable inputs. Value changes enqueue their users and conditional terminators; constant branches/switches become direct edges, unreachable blocks are removed and surviving phi predecessor lists are pruned. Residual unknown values in reachable cycles are conservatively overdefined. It does not infer constants from memory or across calls.

Integer evaluation uses unsigned bit patterns and explicit width masks for wrapping add/subtract/multiply/negation. Signed comparisons and extensions use explicit sign interpretation. Zero divisors, signed minimum/-1 division, and out-of-range shifts are left unfolded rather than evaluated with C++ undefined behavior. Volatile/atomic loads, stores, calls and inline asm remain unknown/effectful; only genuinely unreachable CFG paths disappear. Dedicated IR tests retain volatile, atomic and asm instructions, emit that test with both backends, exercise all eight integer types, O1 exclusion, masks/casts and constant phi joins. The new execution fixture runs on LLVM/custom O0/O2 and all three guests O0/O2.

## Measurements before and after

Same 71 programs in all five configurations at O2, with multi-module `-fwhole-program`: 355 measurements and 142 native execution checks per run, all passed. Both runs passed QEMU instruction-count calibration and accounting checks. Counts are deterministic executed instructions, not cycles. Static totals sum independently emitted programs, including repeated rooted assert/panic code.

| Configuration | Code bytes | Executed instructions | Spill slots | Summed reserved frame bytes |
| --- | ---: | ---: | ---: | ---: |
| small | 83,422 → 73,620 (-11.75%) | 173,416 → 168,921 (-2.59%) | 405 → 371 | 2,040 → 1,916 |
| unreal | 83,416 → 73,614 (-11.75%) | 173,380 → 168,885 (-2.59%) | 405 → 371 | 2,040 → 1,916 |
| unreal32 | 89,195 → 79,144 (-11.27%) | 170,970 → 166,498 (-2.62%) | 391 → 359 | 2,070 → 1,950 |
| custom em64t | 89,064 → 78,259 (-12.13%) | 95,333 → 92,191 (-3.30%) | 22 → 22 | 11,120 → 11,112 |
| LLVM x86_64 | 36,209 → 36,209 | 26,775 → 26,775 | 343 → 343 | 10,584 → 10,584 |

Representative savings: small `control-flow-switch-exec` main executes 49,691 → 47,284; unsigned division helper 40,758 → 40,494; bitfield-ops main 345 → 86 and 1,954 → 545 bytes. Em64t switch main executes 47,413 → 46,162; bitfield-ops main 484 → 106 and 2,292 → 592 bytes. Narrow-ALU main collapses to a constant result (em64t 1,436 → 14 bytes), matching behavior already obtained by LLVM. Full changed-function rows are in `measurement-diff.json`; complete rows and categories in `measure-before/functions.csv` and `measure-after/functions.csv`.

Core5 stage 2 was compiled read-only with the exact requested custom/i8086/unreal/O2/whole-program/loader_entry/include flags for -S, plus -c to obtain function-symbol sizes. Its code shrinks **888 → 867 bytes (-2.36%)**. Largest functions: character render 370 unchanged; __assert 210 → 196; render(slice,color) 121 unchanged; render(slice) 114 unchanged; loader_entry 73 → 66. Main SHA256 remains `d60c82d8a6df94836fcd4da846d79a0093fa7874ee0542b14b25547eba09a3ec`. Listings, objects and complete names are in real-before/ and real-after/. No real-program execution was performed.

## Remaining gap and revised priorities

Em64t is **2.16× LLVM code bytes** (78,259/36,209) and **3.44× executed instructions** (92,191/26,775). Excluding the dominant control-flow-switch program, the executed ratio is still 3.59×, so this is not just one fixture. Realistic program execution totals (custom/LLVM): sort 10,837/3,538; hash 1,397/296; memory 2,298/656; state machine 769/314; math 299/166. LLVM's measured output is unchanged by these passes because its existing optimizer already performs these transformations.

Inspection: custom sort still stores its incoming slice registers into a backend-created 16-byte slot and reloads them before the loop; scalar replacement removed front-end copies, not all backend ABI materialization. It emits immediate values into registers followed by ADD/COPY, and a zero-index GEP still becomes materialize-zero/IMUL/ADD. LLVM uses immediate/LEA forms, folded scaled addresses and hoisted bases. In control-flow-switch, LLVM uses small value lookup tables, four-way loop unrolling and vectorized fixed-trip bands; custom retains scalar case blocks and per-iteration moves/branches. LLVM also vectorizes sort's initialization loop; its insertion routine itself remains scalar. These observations are in retained program.s/program.mir files, not inferred solely from totals.

Estimated gains below are investigation ranges, not promises; they overlap and must not be added. Correctness and a newly approved byte baseline remain prerequisites for each implementation step.

| Rank | Opportunity and evidence | Where | Estimated code / executed instruction gain | em64t benefit | Risk |
| --- | --- | --- | --- | --- | --- |
| 1 | Allocation/coalescing/rematerialization/live-range splitting; DI is permanently excluded from the five-register i8086 allocation pool. Small still executes 54,256 spill stores/reloads (32.1%) and 24,820 register copies (14.7%). The signed-helper residual is a concrete phi/pair case. | shared backend/x86/regalloc.cppm; i8086/registers.cppm policy; pair-aware isel | i8086 3–10% / 8–18%; em64t 2–5% / 2–6% | yes; em64t has fewer spills but 11,813 copies | high: pair interference, flags, scratch/ABI clobbers and mixed widths |
| 2 | Immediate/address folding and ABI-boundary materialization; remove zero GEP arithmetic and redundant parameter-header stores/reloads. Native sort provides direct examples. | backend/i8086/isel.cppm and backend/em64t/isel.cppm; shared MIR cleanup where genuinely common | 3–8% / 3–10% | yes | medium: 16-bit addressing/segments, carry chains and memory access widths |
| 3 | CSE/GVN, loop-invariant code motion and ordinary load/store elimination with alias/effect analysis. Repeated address formation remains in hash/sort loops; promotion exposes more scalar opportunities. | shared dcc.ir.transforms.cppm plus IR analyses; isel for introduced redundant loads | 2–5% / 3–10% | yes | medium/high: volatile/atomic/asm barriers, aliases and increased register pressure |
| 4 | Loop strength reduction and switch-result tables/range simplification. Switch workload alone executes 47,284 small and 46,162 em64t instructions versus 13,937 LLVM; LLVM also unrolls and vectorizes, so this is not all attributable to one technique. | shared loop/CFG analyses; target switch isel; later em64t vectorization | 1–5% / 3–12% on this corpus; unrolling can grow code | yes; SIMD is host-only, not an i8086 proposal | medium/high: wrapping induction, trip counts, signedness, code growth |
| 5 | Branch layout, conditional inversion and redundant jump removal. Small still executes 17,231 branches; em64t 22,224. Adjacent conditional-plus-unconditional patterns remain. | MIR layout/peephole and backend branch emission | 1–3% / 1–5% | yes | low/medium: fallthrough, flags and displacement relaxation |

Width-aware narrow copies/slots and basic scalar replacement/combining/SCCP are now implemented; do not rank their original projected savings again. Remaining width work should be justified by concrete MIR cases. Full CSE/LICM, register allocation changes, fastestcall, strict -farch 8086 and SIMD are not implemented by this batch.

---

# Shared IR scalar replacement (2026-10-15)

This batch validates and extends the random differential generator, then adds scalar replacement before mem2reg in the shared O1/O2 IR pipeline. It does not change O0 or either calling convention. Complete sources, commands, mutation compilers, individual results, per-function measurements and byte proofs are retained in `~/.cache/dcc-sroa-batch/`.

## Mutation validation

Each experiment reintroduced exactly one historical custom-backend defect in a detached scratch worktree, preserving LLVM O0 as an independent oracle. The six fixed seeds plus seeds 1000–1299 ran against each mutant before and after extending the generator.

| Reintroduced defect | Original generator detected | Extended generator detected |
| --- | ---: | ---: |
| 7b21368: narrow sign extension before comparison | 0/306 | 306/306 |
| 5fd0863: aggregate stores to globals | 0/306 | 306/306 |
| b703926: i8086 GEP on aggregate/array values | 0/306 | 304/306 |

All six fixed seeds detect all three defects. Narrow casts and global stores fail custom em64t O0/O2. Aggregate destructuring fails i8086 small/unreal O0/O2; unreal32 does not expose the same width defect. Seeds 1023 and 1279 mask the aggregate mutant's effect, illustrating that a passing individual seed does not prove absence of a bug. A separate narrow-cast control changed its expected exit 5 to 14/12 with that mutation. No fault was applied to the main checkout or committed.

`b8aefb0 feat: extend differential coverage for narrow casts and aggregate storage` adds negative i8 widening/comparisons, global struct/slice stores and reads, struct-pattern destructuring, internal/C aggregate returns, and branch/loop updates of nonescaping structs, constant-index arrays and slice headers. The generator remains bounded structured generation: no inline asm, atomics/concurrency, generic types, floating point, far/based pointers, arbitrary aliasing, padding inspection, arbitrary CFGs or indirect callbacks. Shared frontend bugs can affect both oracle and target; dedicated expected-result fixtures remain necessary.

## Promotion and limits

The pass follows an entry-block single-instance aggregate alloca through constant GEPs, decomposes accesses into typed scalar slots, then lets mem2reg produce SSA values and merge phis. It handles nested structs, fixed arrays and slice headers, including whole-value loads/stores. Extract/construct folds remove temporary reconstructions; nested unchanged by-value parameters forward directly. Function signatures and call argument/return aggregate types remain intact for internal and C ABIs. Pointer flavor/segment and target-sized slice lengths are preserved.

Limits: at most **64 bytes**, **16 scalar leaves**, **16 elements per array**, and **8 nesting levels**. Overlapping layouts, unions, bitfields, flexible arrays, zero-sized or oversized aggregates remain in memory. Any address escape (including passing a field address), pointer cast/phi, volatile/atomic access or nonconstant index rejects the whole candidate. Slice element storage remains separate: promoting a header does not promote dynamically indexed backing data. Parameter forwarding stays conservative around instruction-valued operands inside unattached constant aggregates.

Backend unit checks cover promotion, nested parameter forwarding, nested constant users, and retention for volatile, dynamic-indexed, large and escaping storage. The extended execution generator covers control-flow merges and calls/returns through both conventions; the existing ABI and QEMU matrices remain required.

## O2 measurements

Same 71 programs and five configurations before/after: 355 successful measurements each and 142 native executions each. QEMU instruction callbacks provide deterministic executed counts; both runs passed calibration, prefix decoding and accounting checks. All builds use `-fwhole-program`. The before compiler is 42a677a; unlike the previous width report, it always retains the built-in assert path. Static totals count emitted copies across all programs, including repeated assert/panic functions. They are not a deduplicated library size or a cycle-speedup claim.

| Configuration | Code bytes | Executed instructions | Spill slots | Summed reserved frame bytes |
| --- | ---: | ---: | ---: | ---: |
| small | 93,726 → 83,422 (-11.0%) | 177,068 → 173,416 (-2.1%) | 530 → 405 | 8,320 → 2,040 |
| unreal | 93,720 → 83,416 (-11.0%) | 177,032 → 173,380 (-2.1%) | 530 → 405 | 8,320 → 2,040 |
| unreal32 | 107,585 → 89,195 (-17.1%) | 179,054 → 170,970 (-4.5%) | 373 → 391 | 11,552 → 2,070 |
| custom em64t | 106,473 → 89,064 (-16.4%) | 100,068 → 95,333 (-4.7%) | 22 → 22 | 19,328 → 11,120 |
| LLVM x86_64 | 38,481 → 36,209 (-5.9%) | 26,775 → 26,775 | 343 → 343 | 13,992 → 10,584 |

Important local tradeoff: unreal32 gains 18 spill slots despite a much smaller total frame. Promotion removes ordinary aggregate slots but can expose more scalar live ranges; spill count alone is not a total-stack metric. Small/unreal `__dcc_divmod64` grows 689 → 778 bytes and executes 1,992 → 2,212 instructions in the i64 matrix; the matrix's `memory(Wide)` grows 105 → 157 bytes and executes 45 → 58. These wide-value register-pressure regressions remain visible rather than being excluded from the totals. No measured em64t function grows or executes more instructions.

Representative executed savings: insertion sort small 22,066 → 18,870 (-14.5%), unreal32 24,178 → 17,197 (-28.9%), em64t 14,193 → 10,003 (-29.5%); FNV small 1,431 → 1,111, unreal32 1,627 → 919, em64t 1,116 → 791. Sort's em64t code shrinks 325 → 231 bytes and frame 32 → 16; its small frame shrinks 20 → 8 even though spills grow 2 → 4. Nested-slice fixture small main shrinks 335 → 221 bytes, 137 → 86 executed instructions. Full before/after rows are in `measurement-diff.json`, `measure-before/functions.csv` and `measure-verified/functions.csv`.

Em64t changes intentionally: nonescaping aggregate memory slots, GEPs and reloads disappear; scalar values/phis and aggregate argument forwarding replace them. Sort/hash improve particularly from keeping slice headers in registers. The existing allocator still spills some promoted values; this pass does not add coalescing, rematerialization or a new ABI. LLVM already removes most executed aggregate traffic, so its executed corpus count is unchanged; emitted panic code and reserved frames still shrink.

## Core5 program (measurement only)

The user's Stage 2 `Main.dc` and imported project files were not modified. Exact requested custom/i8086/unreal/O2/whole-program/loader_entry/include flags were used for `-S`; an additional `-c` invocation supplies function-symbol byte sizes without linking or running the program. Disabling only `sroa` reproduces both original assembly and object byte-for-byte. Main source SHA256 remains `d60c82d8a6df94836fcd4da846d79a0093fa7874ee0542b14b25547eba09a3ec`.

Total function code **941 → 888 bytes (-5.6%)**; five functions:

| Function | Before bytes | After bytes |
| --- | ---: | ---: |
| VGA render(character, color) | 370 | 370 |
| __assert | 233 | 210 |
| VGA render(slice, color) | 136 | 121 |
| VGA render(slice) | 129 | 114 |
| loader_entry | 73 | 73 |

Retained listings, objects and symbol sizes: `real-before/` and `real-verified/`. No program execution or cycle-speedup claim is made for this separate real-program measurement.

## Verification result

The final fixed six seeds and 300 extra healthy seeds (2000–2299) passed all 2,754 executions without mismatches. Backend units passed 823/823; the i8086 differential/QEMU matrices passed 546 executions and 150 byte-identical listing/object linked images at O0/O2. Linux passed 104 OS checks, 176 layout assertions and 25 ABI pairs; Windows passed 105 OS checks, 371 layout assertions and 25 ABI pairs, including both Windows compiler backends. Their snapshot has identical compiler sources to the final commit.

The full gate ran **once**, LLVM enabled, with zero skips: 35/36 suites passed, including dispatcher 2219/2219. Its sole failure was the pass-registry unit still requiring three function passes after this feature added a fourth. The test now explicitly checks `sroa`, `mem2reg`, `dce`, `simplifycfg` and singleton registration; the corrected targeted suite passes **133/133**. All other suites passed in the full run. The full gate was not repeated. This correction changes only a test expectation, not compiler output.

---

# Width-aware i8086 copies and spill slots

The seeded generator and the first approved allocation-width step are complete. The native byte baseline was first brought forward from 4422e5c to b7bbaff by isolated captures at every intervening batch endpoint. All 583 changed and 918 added artifacts are attributed; all 130 changed objects are named with their introducing commits and reasons in `~/.cache/dcc-width-batch/batch-attribution.md` and `.json`. Two independent HEAD directories produced identical 30,773-record, 49,925,327-byte captures. The preserved baseline is `~/.cache/dcc-backend-baseline/baseline-b7bbaff-a`, with independent companion `-b`. The end-of-output-batch baseline adoption rule is recorded in CLAUDE.md.

## Generator

`77a6090 feat: add seeded cross-backend execution differential` adds `tools/codegen/random_differential.py` and its six-seed `make test` invocation. Each seeded DC program exercises all eight integer types, narrow and wide arithmetic, comparisons, branches, bounded loops, structs, arrays, slices, pointers to locals/globals/fields, and mixed one-, three- and nine-argument calls. Runtime inputs and noinline runtime helpers prevent the tests becoming entirely compile-time computations. All logical results feed a target-independent u32 wrapping checksum, printed as eight hexadecimal digits. Operands, divisions, shifts and indices are constructed to avoid undefined behavior.

The oracle is LLVM x86_64 O0; comparisons cover custom em64t O0/O2 and i8086 small/unreal/unreal32 O0/O2 under QEMU TCG. Each configuration must exit successfully and print the identical checksum. Failures retain the seed, source, commands' diagnostics and configuration results; optional greedy reduction removes independent checksum blocks. It leaves unused helpers, so it is a reduced reproducer rather than a complete language reducer. The fixed six seeds took about five seconds here. The 300-seed pre-change and final post-change runs (0–299, 2,700 executions each) found **no mismatches**, with identical reference checksums before/after. The pre-change fixed set also passed all 54 executions; no compiler defects were fixed as part of generator work. Larger manual runs and reproduction commands are in README.md.

## Allocation-width change

The i8086 register policy opts into width inference; em64t keeps the existing policy. After phi elimination, a worklist propagates required widths backward through virtual-copy chains. Actual machine operations, extension source widths and 16/32-bit memory addresses determine the requirements. Word-only masks and left shifts that discard the input's upper half can consume narrow inputs. Narrow copies become MOV16rr; narrow virtual ranges spill/reload with MOV16mr/MOV16rm into aligned two-byte slots. Eight-bit values retain their normalized 16-bit representation. Dword values, i64 dword components, wide addresses and full-register ABI saves stay full width.

Parallel-copy resolution retains individual widths and preserves the largest required width when breaking a mixed cycle; narrow-only cycle temporaries use two bytes. Narrow copies participate in physical-register read constraints and the existing self-copy removal. Focused tests exercise u8/u16 versus u32 spill storage and simulate mixed-width physical cycles with poisoned upper halves. Reserved scratch-register saves retain full width because they preserve physical register contents rather than one typed narrow virtual value. No ABI or shared IR pass changed.

## Before and after at O2

Same 71 programs, 355 configurations, 142 additional native checks; multi-module builds use `-fwhole-program`. Every measurement passed, including QEMU callback calibration and category/prefix accounting. Counts use the existing deterministic instruction plugin, with BIOS/startup excluded; they are instructions reached, not cycles.

| Configuration | Code bytes | Executed instructions | Spill slots | Reserved frame bytes |
| --- | ---: | ---: | ---: | ---: |
| small | 76,198 → 74,201 (-2.62%) | 177,423 → 177,068 (-0.20%) | 388 → 388 | 2,734 → 2,356 (-13.83%) |
| unreal | 76,192 → 74,195 (-2.62%) | 177,387 → 177,032 (-0.20%) | 388 → 388 | 2,734 → 2,356 (-13.83%) |
| unreal32 | 78,566 → 76,771 (-2.28%) | 179,385 → 179,054 (-0.18%) | 373 → 373 | 2,782 → 2,464 (-11.43%) |
| custom em64t | 68,701 → 68,701 | 100,068 → 100,068 | 22 → 22 | 1,152 → 1,152 |
| LLVM x86_64 | 24,565 → 24,565 | 26,775 → 26,775 | 343 → 343 | 1,496 → 1,496 |

Small/unreal now have 189 two-byte and 199 four-byte spill slots; unreal32 has 159 two-byte and 214 four-byte slots. Counts do not decrease: this step reduces transfer width and storage, rather than choosing a different allocation algorithm. Total frame bytes are summed over emitted functions, not peak stack usage.

| Prefix66 bytes | small | unreal | unreal32 |
| --- | ---: | ---: | ---: |
| Static | 12,889 → 11,069 | 12,885 → 11,065 | 13,610 → 11,972 |
| Execution-weighted | 108,837 → 92,903 | 108,837 → 92,903 | 123,330 → 112,339 |

Small spill-store/reload execution counts stay 16,682/35,319. Copies fall 24,409 → 24,054 through the existing self-move cleanup now recognizing narrow copies; other exclusive executed categories stay unchanged. Prefix bytes overlap instruction categories. Width changes primarily save bytes and frame space; no cycle-speedup claim follows from these counts.

Largest small-model code savings: `i64-pairs-matrix-exec::dcc_main` 11,468 → 10,968 bytes, frame 448 → 264; i8 matrix 6,706 → 6,448; u8 matrix 5,888 → 5,636; i16 matrix 5,797 → 5,563; u16 matrix 5,329 → 5,101. Full per-function before/after code, executed instructions, spill counts and frame sizes are in `~/.cache/dcc-width-batch/measurement-diff.json`; complete measurements, objects, MIR, slot widths and instruction callbacks are in `measure-before/` and `measure-final/` beside it.

## Verification

Final targeted unit checks: 762/762. QEMU differential list: 66 fixtures × three models × O0/O2 = 396 executions. ABI, based/far, memory and i64 matrices: 150 further O0/O2 executions and 150 byte-identical direct-object versus assembled-listing linked images. All 546 executions passed. The final random, native byte comparison, end-of-batch integration and adopted baseline identities are recorded in AGENTS.md and the batch completion report. Retained evidence is under `~/.cache/dcc-width-batch/`. No output changes are hidden by updating expected execution results.

The earlier division report follows as historical evidence; its width optimization proposal is implemented above. Remaining broad opportunities still begin with aggregate scalar replacement/memory elimination and allocation quality (coalescing, rematerialization, splitting), followed by addressing/immediate folding and redundant memory operations. This batch implements no further optimization step.

---

# Code generation measurements after i8086 division fast paths

The approved fast paths halve the corpus's i8086 executed instruction count. Small-model execution falls **356,707 → 177,423 (-50.3%)**, with **75,220 → 76,198 bytes (+1.3%)**. Unreal behaves equivalently; unreal32 falls 50.0% with 1.4% more text. Custom em64t and LLVM code bytes and instruction counts are unchanged.

The remaining optimization ranking below uses the **non-helper workloads** as its primary evidence, rather than weighting priorities toward i64 division. Division helpers still account for 34.6% of small-model execution, down from 67.4% including signed normalization before the change. The retained wide-divisor shift/subtract path is expected to remain expensive. No further optimization or byte-baseline adoption was performed.

## Changes and commits

- `19ab5a4 fix: print word width for em64t u16 extending loads`: the MOVZX64rm16 assembly-printer path used `RegWidth::Bits8` for memory operands. It now prints `word`; byte loads still print `byte`. The new fixture checks both instructions and high-u16 execution. The object encoder was already correct.
- `e4423c3 feat: accelerate i8086 i64 division with chained dword divides`: a private compiler-owned `__dcc_udivmod32(high, low, divisor)` operation lowers to one unsigned 32-bit DIV, returning quotient/remainder as the low/high dwords of a u64. Input and output register transfers use parallel-copy groups and DIV explicitly defines/uses EAX/EDX.

The unsigned helper checks zero first. If the divisor fits in 32 bits and the dividend does too, it invokes the primitive once with a zero high dword. Otherwise, with a 32-bit divisor, it divides the high dword first, then divides `first_remainder:low_dword`. Since the first remainder is less than the divisor, the second DIV's quotient fits in a dword. Quotient dwords are combined; the second remainder is the final remainder. Divisors wider than 32 bits retain the original shift/subtract loop.

Signed handling remains unchanged: normalize magnitudes in unsigned arithmetic, run the unsigned helper, negate the quotient for unlike signs and the remainder for a negative dividend. Zero and signed INT64_MIN/-1 still produce #DE, including remainder operations. The helper's public calling convention and result layout are unchanged.

## Printer artifact audit

Fresh captures before and after the isolated printer fix changed **226 existing assembly artifacts**: 162 fixture listings, 56 libdcext listings and 8 ABI listings. [Complete artifact list](printer-artifacts.txt). Every changed line is solely a MOVZX memory operand changing `byte` to `word`. All common object/status/other artifacts are byte-identical; no records were removed. There are 21 additional records for the new fixture. These captures did not replace the approved baseline.

Targeted checks compiled ELF/COFF at O0/O2, verified direct objects were unchanged, assembled all four listings and executed the two ELF listings successfully with a value above 255. The measurement's original high-u16 reproduction is now covered by the regression shape.

## Measurement method

Same **71 programs**, all 66 i8086 differential-list fixtures plus five realistic multi-module workloads. Every measurement compilation uses `-O2 -fwhole-program`. Custom: i8086 small/unreal/unreal32 and em64t; LLVM: x86_64-elf. All **355/355 before and 355/355 after** measurements passed their expected results; each run also passed 142 native x86-64 checks.

Executed instruction counts use the QEMU 11.1.2 API-7 per-instruction plugin, TCG with one guest CPU, and qemu-x86_64 for the flat builds. Native perf is unavailable. Startup/BIOS instructions are excluded; actually referenced freestanding runtime functions are included. REP copying four bytes generates one callback, as calibrated on both architectures. Counts measure instructions reached, not cycles, micro-ops or REP's internal work. Definitions and reproduction commands are in [README](README.md).

Retained evidence under `/home/admin/.cache/dcc-div-fastpaths/`: `measure-before/`, `measure-after/` (JSON, CSV, summary, per-address counts, objects, disassembly inputs, MIR and maps); `printer-artifact-differences.json`; `instruction-paths.json`; `regression-runtime-proof.json`; `random-final/`; build and integration logs. The measured after-source matches the division commit; compilation occurred just before committing it, so the JSON retains its parent revision plus the exact compiler source diff.

## Before and after

| Configuration | Code bytes before → after | Change | Executed instructions before → after | Change |
| --- | ---: | ---: | ---: | ---: |
| i8086 small | 75,220 → 76,198 | +1.3% | 356,707 → 177,423 | -50.3% |
| i8086 unreal | 75,214 → 76,192 | +1.3% | 356,671 → 177,387 | -50.3% |
| i8086 unreal32 | 77,500 → 78,566 | +1.4% | 358,669 → 179,385 | -50.0% |
| Custom em64t | 68,701 → 68,701 | 0% | 100,068 → 100,068 | 0% |
| LLVM x86_64 | 24,565 → 24,565 | 0% | 26,775 → 26,775 | 0% |

Custom em64t's gap remains **2.80× LLVM's code bytes and 3.74× its executed instructions**. Its emitted objects are unaffected by either change.

After-change i8086 executed categories:

| Category | small | unreal | unreal32 |
| --- | ---: | ---: | ---: |
| Spill stores | 16,682 | 16,682 | 16,096 |
| Spill reloads | 35,319 | 35,319 | 33,020 |
| Register copies | 24,409 | 24,395 | 23,930 |
| Frame/stack operations | 4,496 | 4,496 | 4,488 |
| Branches and returns | 17,385 | 17,385 | 17,385 |
| Calls | 364 | 364 | 364 |
| Everything else | 78,768 | 78,746 | 84,102 |
| 0x66 execution-weighted prefix bytes (overlapping) | 108,837 | 108,837 | 123,330 |
| 0x67 execution-weighted prefix bytes (overlapping) | 0 | 22 | 9,375 |

Small spill traffic falls **134,225 → 52,001** instructions and copies **46,441 → 24,409**. Spills still account for 29.3% of the new total. Narrowing widths alone does not reduce instruction count: prefix bytes overlap the categories and must not be added to them.

There is a stack/text tradeoff: the unsigned helper grows **1,055 → 1,544 bytes** in small/unreal, with **28 → 33 spill locations and 128 → 180 reserved frame bytes**. Unreal32 grows **1,078 → 1,611 bytes**, with 32 spill locations and a 176-byte frame. Across all functions, small/unreal spill locations grow 378 → 388 and reserved frame totals 2,630 → 2,734; unreal32 grows 365 → 373 and 2,686 → 2,782. Frame sums are not peak runtime stack usage. Allocation and width work remain useful even after the execution improvement.

## What improved, and what remains

The i64 matrix's unsigned helper falls **220,044 → 40,762** executions; its signed wrapper remains 2,004. The realistic integer program uses a divisor of `2^32 + 1`, so its unsigned-helper count stays approximately unchanged (**18,540 → 18,538**): it intentionally exercises the retained fallback.

QEMU callback probes independently verify all three models:

| Shape | Executed hardware DIVs in unsigned helper | Small/unreal helper instructions | Unreal32 helper instructions |
| --- | ---: | ---: | ---: |
| UINT32_MAX / 3 | 1 | 100 | 101 |
| UINT64_MAX / 3 | 2 | 109 | 110 |
| UINT64_MAX / (2^32 + 1) | 0 | 10,238 | 10,239 |

Excluding compiler arithmetic helpers, small executes **116,119 instructions**: 25,075 spill operations (21.6%), 16,789 register copies (14.5%), 15,327 branches/returns (13.2%) and 54,708 other instructions (47.1%). Unreal32's corresponding total is 118,003. This subset is unchanged by the helper optimization and is the basis for the revised priorities.

Largest remaining non-helper functions:

| Function | small executions | unreal32 executions | custom em64t executions | LLVM executions |
| --- | ---: | ---: | ---: | ---: |
| switch workload `dcc_main` | 49,691 | 49,655 | 47,413 | 13,937 |
| insertion-sort routine | 22,066 | 24,178 | 14,193 | 3,217 |
| branches workload `dcc_main` | 4,347 | 4,347 | 4,456 | 907 |
| recursive Fibonacci | 4,154 | 4,154 | 4,063 | 1,740 |
| inclusive-u8 loop | 3,850 | 3,850 | 4,357 | small LLVM loop/constant simplification |

Switch and sorting together consume **61.8%** of small's non-helper work. The switch has 13,403 spill accesses and 6,014 copies. Sorting has 3,226 spills and 2,141 copies on small, while custom em64t sorting has **no allocator spills**: its overhead includes repeated stack slice copies, pointer/length reloads and boolean/CFG scaffolding. LLVM keeps the slice in registers, reuses the loaded element, branches directly and unrolls selected loops.

Static hotspots are unchanged: i64 matrix `dcc_main` 11,468 bytes, i8 matrix 6,706, i32 matrix 6,094 and u8 matrix 5,888 in small. The shared custom pipeline still has inline, mem2reg, DCE and simplifycfg, rather than general SCCP/GVN/LICM. LLVM's CSE of repeated nonvolatile matrix calculations, scalar replacement, immediate/address folding and loop transforms explain much of the remaining gap. LLVM vectorization/unrolling sometimes increases text: the hash program is 426 bytes versus custom em64t's 384, but executes 296 versus 1,725 instructions.

## Revised ranking of remaining opportunities

Estimates are planning ranges, **not measured improvements**, and overlap. The instruction estimates below use the **116,119-instruction small non-helper subset**, so the retained division loop does not distort priorities. Whole-corpus reductions from changes confined to this subset would be about 65.4% of the listed percentage. Additional helper benefits, if any, are excluded from these estimates. Text estimates use whole-corpus function bytes. Each step needs its own approval and measured validation.

| Rank | Opportunity and evidence | Estimated text / non-helper instruction gain | Location, em64t benefit and risk |
| --- | --- | --- | --- |
| 1 | **Scalar replacement and redundant memory elimination.** Sorting repeatedly copies slices and reloads pointer/length/previous elements. Its 22,066 instructions are 19.0% of the subset; em64t's spill-free routine still uses 4.41× LLVM's instructions. Start with proven nonescaping aggregates and local load forwarding/CSE. | 2–6% text; 5–12% instructions, with 20–50% routine-local savings plausible. | Shared IR SROA/promotion and alias-aware memory optimization. Strong em64t benefit. Medium/high risk: escapes, aliasing, volatile/atomic access and far/based layout. |
| 2 | **Width-aware copies and spill slots.** i8086 still uses four-byte GPR spills/COPY for u16 values. Non-helper copies are 14.5% of execution and still carry unnecessary full-width moves. Width reduction can shrink prefixes and stack displacements, even when instruction counts stay constant. | 3–8% text; 0% instructions for width substitution alone, 0–2% from eliminating proven redundant extensions. | MIR width metadata, i8086 selection/policy/frame layout and shared allocator. Some em64t benefit with different zero-extension rules. Medium risk: upper-half liveness, mixed-width phis, parallel copies, C ABI full ESI/EDI preservation. |
| 3 | **Allocation quality: coalescing, rematerialization, live-range splitting.** Non-helper spills are 21.6% of execution; switch and branch-pressure functions dominate them. Copies remain 14.5%. Current linear scan has fixed-order interval assignment and no general rematerialization/splitting. | 3–8% text; 8–16% instructions by eliminating part of spill/copy traffic. | Shared allocator. Em64t benefits most in pressure cases; branches alone executes 1,748 spills. High risk: physical clobbers, CL shifts, DIV and register-family aliases. Evaluate freeing permanently reserved DI as a separate later experiment with scoped scratch reservations; it cannot simply be added to the allocatable set. |
| 4 | **General constant propagation, CSE and algebraic/CFG simplification.** Large matrices repeat arithmetic, divisions, shifts and comparisons from values read once from volatile globals. Switch consumes 42.8% of the subset; LLVM executes 3.40× fewer instructions on em64t for that program. Avoid recomputation and unnecessary boolean merge/test scaffolding. | 8–20% text; 5–15% instructions. | Shared IR SCCP/local CSE, followed by dominance-aware GVN and CFG simplification. Strong em64t benefit. Medium/high risk: narrow wrapping, trapping operations, memory invalidation and side effects. Do not merge volatile reads or speculate a short-circuit RHS. |
| 5 | **Immediate and addressing-mode folding.** Constant temporaries, increments followed by copies, and multiply-by-two/base-add sequences remain. Em64t already folds scaled addresses but still has 312 flag-preserving short-immediate opportunities, bounded at 1,424 bytes. | 2–5% text; 2–6% instructions. | Isel/MIR peephole and immediate encoding. Both backends. Low/medium risk for moves; medium for flags, displacement limits and legal 16-bit DS/SS addressing. |
| 6 | **LICM and strength reduction.** Slice/global addresses and lengths remain in loops; small-model indices are repeatedly scaled and transferred to legal address registers. Pointer induction and invariant promotion can reduce sorting/hash/memory work. | 1–4% text; 3–8% instructions. | Shared IR loop analysis and induction simplification; target-aware isel. Benefits em64t too. High risk for memory LICM and zero-trip/trapping expressions; medium for wraparound/0xFFFF and far offsets. |
| 7 | **Redundant MIR moves, addresses and cleanup.** 56 narrow small self-moves occupy 112 bytes and execute 375 times; identical adjacent LEAs remain. Existing cleanup only handles COPY/MOV64rr self-moves. | Proven self-moves: 0.15% text / 0.32% subset instructions. Broader cleanup: 0.5–2% text / 0.5–3% instructions. | Shared MIR peephole after allocation. Both backends. Low risk for full-value copies; medium for extensions and flags. Preserve em64t 32-bit writes unless upper-bit effects are proven irrelevant. |
| 8 | **Branch layout/fusion/relaxation.** i8086 already removes fallthrough jumps and fuses comparisons; the old 244 short-branch candidates give a 389-byte bound, without reducing instruction count. Em64t retains 149 next-instruction jumps (745 bytes, 772 executions), and roughly 3.2 KB of conservative shortening opportunity. | About 0.5% i8086 text for the measured shortening set, 0% instructions from shortening; further layout/fusion may save 1–3% subset instructions. | MIR layout plus iterative encoder relaxation. Larger immediate em64t benefit. Medium risk: CFG/flags, branch-distance boundaries, jump tables and relocation correctness. |
| 9 | **Frame omission and later fastestcall.** Non-helper frame/stack operations are 3,868 (3.3%); calls 352 (0.3%). Aggregate argument/sret movement and conservative call clobbers add indirect cost. Preserve the agreed roadmap: fastestcall follows i8086 optimization. | 1–4% text; 1–3% instructions before secondary allocation effects. | ABI/frame layout with whole-program SCC/address-taken analysis. Both backends. High risk for fastestcall, recursion, C interop, preservation, segment state and unwind/debugging. |

Divmod-result reuse and improvements to the remaining wide-divisor algorithm are separate, lower-priority follow-ups rather than drivers of this ranking. The realistic math program still invokes the fallback twice for identical division/remainder operands; reuse could save roughly half its helper work, but that is localized evidence and needs trap/evaluation-order checks. No such reuse was added here. Vectorization/unrolling remains a later em64t opportunity with an explicit size tradeoff, and strict `-farch 8086` remains after fastestcall.

## Correctness verification and baseline policy

Targeted verification passed:

- **48** i64 matrix, NASM C ABI, fast-path and #DE runs, all three models at O0/O2. Every listing reassembled to the same linked image. Zero divisor, signed overflow, remainder-zero and native #DE fixtures all passed.
- **13,968 operand pairs**: 8,192 seeded random pairs and 5,776 boundary cross-products, including every power of two, 0, 1, -1, 2^32±1, INT64_MIN and UINT64_MAX. For each of the 13,892 nonzero-divisor pairs, QEMU directly compared new results with the embedded old helper; signed MIN/-1 was reserved for the trap fixtures. All actual quotient/remainder values were then compared byte-for-byte with both LLVM and custom native x86-64 results.
- All six model/optimization runs produced the same **444,544-byte result stream**. SHA256 `3d2ce49ff7aff218ef458299a673da461602e3dcb2effef21369abd5275d6914` summarizes those identical streams; comparison used the full bytes, not only the hash. Seed `0xC0DEF00D`; reproduce with `tools/codegen/div_differential.py`.
- Nine execution-count probes confirmed the one-DIV/two-DIV/fallback distinction in all models. Measurement accounting/prefix/plugin calibration checks passed.

For subsequent optimizations, retain execution-based verification: the i8086 differential list at O0/O2 in all models, ABI interop, QEMU memory/far/based/i64/trap matrices and em64t execution fixtures on both backends, including ELF/COFF and Linux/Windows ABI checks when applicable. Add pressure/random CFG tests before allocation changes; high-u16/poisoned-upper-half and parallel-copy tests before width changes; alias/volatile/zero-trip/trap tests before memory or LICM changes. The seeded division differential adds substantial arithmetic coverage, but is not a general randomized compiler test generator.

Record before/after byte, instruction-category, frame and per-function changes and explain each transformation. Capture proposed replacement byte baselines twice from distinct source directories, preserve the previous baseline and adopt each new baseline **only after explicit user approval**. This batch changes output intentionally but does not adopt a baseline. The roadmap and ignored AGENTS.md record remain i8086 optimization, then fastestcall, then strict 8086.
