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
