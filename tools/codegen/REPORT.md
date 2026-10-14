# Custom backend measurements, 2026-10-14

The largest measured i8086 opportunity is spill/copy pressure in the generated i64 division helper. The portable workloads also expose repeated aggregate/slice traffic and redundant control flow. Custom em64t emits **2.80×** LLVM's function bytes and executes **3.74×** its instruction callbacks on this corpus. These are corpus totals, not a claim about cycle performance or all applications.

The roadmap is recorded in the ignored `AGENTS.md`: general i8086 inline assembly (step 15) and custom inline-assembly improvements are backlog work; module assembly and separate assembly objects cover current needs. Next: custom code generation optimization, i8086 first, then fastestcall, then `-farch 8086`. No optimization or ABI change was implemented, and no byte baseline was adopted.

## Method and validation

Measured compiler base: `a5351c1afc9284f6c915ad95f5f345fd9dad05e2`, plus measurement-only final-frame-slot dumps and optional verbose LLVM assembly. All sources use `-O2 -fwhole-program`, including the five two-module realistic workloads. The corpus contains all **66** differential-list fixtures plus string/memory routines, FNV-1a, insertion sort, token-state scanning and integer math with i64 mixing/division. The realistic inputs come from volatile globals to retain runtime work.

QEMU **11.1.2**, TCG, one guest CPU, with a locally built API-7 per-instruction counting plugin measures both real-mode guests and qemu-x86_64. Native perf was unavailable (`perf` is not installed). Both x86-64 builds also run natively to check results. No BIOS/startup/host runtime instructions are included. Referenced freestanding library-call bodies are included in LLVM's totals. REP calibration reports one callback per REP instruction, independent of its byte count. See README for category, frame and prefix definitions.

Validation:

- Full build passed. No test-suite files changed, so no full gate was run, as requested.
- **355/355** program/configuration measurements passed their expected exit checks; **142/142** x86-64 native execution checks passed.
- Five independent sorting repeats reproduced every static and per-function executed category count exactly.
- The plugin calibration passed for 16/64-bit five-iteration loops and REP; accounting checks confirmed every function's decoded byte total equals its symbol size and categories sum to instruction totals.
- All **355** objects before/after optional LLVM annotations were byte-identical. The custom slot-dump changes affect only requested MIR text; no optimizer or code emitter was changed.
- Python syntax checks, plugin compilation with `-Wall -Wextra -Werror`, and `git diff --check` passed.

Retained artifacts: `/home/admin/.cache/dcc-codegen-measure/final/measurements.json`, `functions.csv`, `summary.md`, individual objects/images/listings/MIR/maps/counts; sibling build logs, `object-proof.json`, repeat results, and printer reproduction. Totals below exclude padding between function symbols and sum each program separately, including each emitted copy of its helpers. Frame totals sum functions; they are not maximum runtime stack use.

## Totals

| Configuration | Functions | Code bytes | Static instructions | Executed instructions | Spill locations | Frame bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| i8086 small | 139 | 75,220 | 22,139 | 356,707 | 378 | 2,630 |
| i8086 unreal | 139 | 75,214 | 22,128 | 356,671 | 378 | 2,630 |
| i8086 unreal32 | 139 | 77,500 | 22,218 | 358,669 | 365 | 2,686 |
| custom em64t | 133 | 68,701 | 18,513 | 100,068 | 22 | 1,152 |
| LLVM x86_64 | 135 | 24,565 | 6,069 | 26,775 | 343 | 1,496 |

LLVM's larger spill-location count comes mostly from straight-line test-matrix code; its spill instructions execute only 799 times. It does not explain custom's loop overhead, and physical LLVM spill addresses are not abstract allocator-object counts. Two referenced runtime routines account for the extra LLVM function entries; i8086 also emits arithmetic helpers.

Each category cell below is **static instructions / executed instructions**. Prefix cells are **static bytes / execution-weighted bytes**, overlap instruction categories and must not be added to them.

| Category | small | unreal | unreal32 | em64t | LLVM |
| --- | ---: | ---: | ---: | ---: | ---: |
| Spill stores | 971 / 49,512 | 971 / 49,512 | 950 / 48,926 | 40 / 739 | 401 / 401 |
| Spill reloads | 2,373 / 84,713 | 2,373 / 84,713 | 2,328 / 82,414 | 58 / 1,009 | 388 / 398 |
| Register copies | 3,367 / 46,441 | 3,364 / 46,427 | 3,304 / 45,962 | 1,797 / 11,959 | 465 / 1,187 |
| Frame/stack operations | 2,239 / 4,496 | 2,235 / 4,496 | 2,170 / 4,488 | 1,284 / 2,404 | 154 / 682 |
| Branches and returns | 1,048 / 23,529 | 1,048 / 23,529 | 1,048 / 23,529 | 1,476 / 22,523 | 478 / 6,910 |
| Calls | 143 / 364 | 143 / 364 | 143 / 364 | 131 / 326 | 121 / 208 |
| Everything else | 11,998 / 147,652 | 11,994 / 147,630 | 12,275 / 152,986 | 13,727 / 61,108 | 4,062 / 16,989 |
| 0x66 prefix bytes | 12,715 / 258,285 | 12,711 / 258,285 | 13,400 / 272,778 | 15 / 571 | 1,392 / 4,160 |
| 0x67 prefix bytes | 0 / 0 | 4 / 22 | 718 / 9,375 | 0 / 0 | 0 / 0 |

Small-model spills consume **13,740 bytes (18.3%)** and **134,225 executions (37.6%)**. Copies consume 9,178 bytes (12.2%) and another 13.0% of execution. Prefix 0x66 alone occupies **16.9%** of small-model text, but many prefixes implement necessary u32/u64 work. They cannot all be removed. Unreal32 is 3.0% larger than small; its wide address operations add 718 static 0x67 bytes and 9,375 weighted executions. Small and unreal are nearly identical because this portable corpus predominantly accesses near conventional-memory objects, not extended-memory far data.

## Where the work goes

Worst functions by execution, small model:

| Program/function | Bytes | Executed instructions | Spills | Reserved frame |
| --- | ---: | ---: | ---: | ---: |
| i64 matrix / `__dcc_udivmod64` | 1,055 | 220,044 | 28 | 128 |
| control-flow switch / `dcc_main` | 1,187 | 49,691 | 12 | 48 |
| insertion sort / `insertion` | 263 | 22,066 | 2 | 24 |
| realistic math / `__dcc_udivmod64` | 1,055 | 18,540 | 28 | 128 |
| control-flow branches / `dcc_main` | 1,399 | 4,347 | 39 | 156 |
| internal calls / `fibonacci` | 91 | 4,154 | 1 | 4 |
| inclusive-u8 loop / `dcc_main` | 98 | 3,850 | 0 | 0 |
| i64 matrix / `dcc_main` | 11,468 | 2,558 | 92 | 448 |

The two division-helper copies account for **238,584 executions, 66.9%** of the total, but only 2,110 static bytes (2.8%). The matrix calls the helper 24 times; realistic math calls it twice. The matrix helper's 220,044 executions contain **100,296 spill operations** and **27,096 register copies**. Its 64-iteration restoring-division loop is in `compiler/source/dcc.vfs.cppm:417`; repeated variable-width multiword shifts and aggregate results amplify allocation pressure. Improving helper code does more for this execution mix than shortening its few branches.

Largest functions by bytes: i64 matrix `dcc_main` 11,468; i8 matrix 6,706; i32 matrix 6,094; u8 matrix 5,888. Large straight-line operation matrices dominate static totals. Unreal32 has the same hotspots: helper 220,068 executions, switch 49,655, insertion 24,178; i64 matrix `dcc_main` grows to 11,563 bytes. Full per-function rankings for every model are in generated `summary.md` and `functions.csv`.

Portable realistic programs, including their callees and referenced runtime bodies:

| Workload | small bytes / executions | unreal32 bytes / executions | em64t bytes / executions | LLVM bytes / executions |
| --- | ---: | ---: | ---: | ---: |
| String/memory | 340 / 2,931 | 401 / 2,863 | 520 / 2,298 | 600 / 656 |
| FNV-1a | 344 / 3,144 | 442 / 3,214 | 384 / 1,725 | 426 / 296 |
| Insertion sort | 525 / 23,394 | 678 / 25,219 | 616 / 15,030 | 509 / 3,538 |
| State scan | 363 / 1,065 | 376 / 1,048 | 416 / 769 | 137 / 314 |
| Integer/i64 math | 2,466 / 19,150 | 2,576 / 19,155 | 395 / 299 | 396 / 166 |

## What LLVM does differently

**Slice/aggregate scalar replacement, CSE and loop hoisting.** Insertion sort's custom em64t entry stores pointer/length into one 16-byte stack slice, copies it to another, then repeatedly forms `[rbp-32]` addresses and reloads the pointer and length inside loops. Consecutive identical LEAs also survive. LLVM keeps the pointer and length in registers and directly addresses `(%rdi,%r9,2)`. Its inner loop loads the previous element once, compares it, stores that same value when needed and decrements the index. Custom reloads the element and slice pointer across its short-circuit blocks. The sorting function is 325 bytes / 14,193 executions custom versus 186 / 3,217 LLVM. There are **no custom allocator spills** in this em64t function: much of the excess is ordinary aggregate memory traffic, not allocation failure.

**Control-flow simplification and induction handling.** Custom em64t often materializes a boolean, jumps to a merge, tests it and jumps again. LLVM branches directly and uses decrement/inc forms and unrolling where profitable. In the switch workload, custom executes 47,413 instructions with 15,200 branches versus LLVM's 13,937 with 4,778 branches. i8086 already fuses single-use comparisons and removes fallthrough jumps; em64t still contains 149 jumps to the immediately following instruction (772 executions, 745 bytes).

**Immediate/operation folding.** Custom emits a register constant for loop increments and comparisons, plus an ALU operation and register copy. Small sort includes `imul index,2` followed by separate base moves/adds; custom em64t already folds many scaled addresses but does not consistently fold arithmetic immediates. A scan found 312 custom em64t MOVABS constants representable by a flag-preserving shorter immediate move: an isolated **1,424-byte upper bound** on savings without deleting instructions. Zero/one materializations execute 7,634/5,174 times on em64t and 13,257/2,219 on small i8086.

**CSE of repeated matrix operations.** The integer fixtures repeatedly calculate the same `a+b`, `a/b`, comparison and shift from values read once from volatile globals. LLVM can reuse those nonvolatile computations. The shared custom pipeline currently registers only inline, mem2reg, DCE and simplifycfg (`dcc.ir.transforms.cppm:3532`); it has no general SCCP/GVN/LICM pass. Frontend constant tracking and inline partial evaluation do exist; general propagation should extend them rather than assume there is no folding today.

**Vectorization/unrolling and loop idioms.** LLVM vectorizes hash-buffer initialization and unrolls the scalar FNV loop fourfold. LLVM's hashing program is slightly larger, 426 versus 384 bytes, while using 296 versus 1,725 callbacks. LLVM recognizes memset/strlen idioms; the measurement includes their freestanding runtime bodies. Those size/count tradeoffs should not automatically become the i8086 policy.

## Ranked opportunities for approval

This ordering balances evidence, effort and risk, starting with changes that make later allocation work meaningful. Estimates are planning ranges against this corpus, not measured optimization results. They overlap and **must not be added**. A callback reduction is not a predicted cycle reduction.

| Rank / opportunity | Evidence and estimated gain | Layer / em64t benefit | Risk |
| --- | --- | --- | --- |
| 1. Width-aware copies and spill slots | i8086 policy fixes GPRs, COPY and spills at 32 bits/four bytes (`backend/i8086/registers.cppm:14-32`), including u16 indices. Small has 12,715 0x66 bytes. Conservatively target **3–8% text**, **0% instruction count for width substitution alone**; deleting newly redundant extensions may add 0–2%. Full prefix count is only an upper bound. | Shared MIR/allocator value-width metadata plus i8086 isel/policy/frame layout. Some em64t benefit from narrower spills/copies, with different extension semantics. | Medium: upper-half liveness, register pairs, mixed-width phi/asm copies, C ABI preserved full ESI/EDI, sign/zero extension and spill alignment. |
| 2. Allocation: coalescing, rematerialization, splitting, scratch borrowing | Small spends 37.6% of execution on spills and 13.0% on copies. Current linear scan assigns one interval, spills by farthest end and uses fixed register order; no general copy-hint coalescing, constant rematerialization or split intervals. Target **5–12% text, 12–25% callbacks** by removing a portion of this traffic. DI is permanently unavailable to allocation, leaving five GPRs, with only SI available across calls. Evaluate allocating DI with scoped scratch reservation separately; do not just add it to the list. | Shared allocator; i8086 register policy. Em64t benefits especially under pressure (branches workload: 1,748 spill callbacks), less on already spill-free loops. | High: physical clobbers/uses, CL shifts, byte/address-register masks, parallel-copy cycles, SI/DI REP operands and register-family aliases. Cost-model/coalescing can precede splitting/DI changes. |
| 3. Aggregate scalar replacement and load/store elimination | Sorting and FNV repeatedly copy/reload slices. Custom sort has no em64t spills yet uses 4.41× LLVM's sorting callbacks. Target **2–6% total text, 3–8% total small callbacks**, with **20–50%** reductions plausible in these slice routines; em64t realistic loops have larger proportional opportunity. | Shared IR SROA/promotion, then alias-aware redundant memory elimination. Benefits both; LLVM already performs much of it. | Medium/high: address escapes, aggregate aliasing, volatile/atomic accesses, far/based pointer layout and segment reads. Promote proven nonescaping fields first. |
| 4. i64 divmod reuse and helper fast paths | Helpers consume 66.9% of small callbacks. Realistic `x/y + x%y` invokes the same helper twice (18,540 callbacks). Reusing one divmod result would approximately halve that helper work. After allocation improvements, evaluate dividend<divisor, proven 32-bit cases and significant-bit loop bounds. Target **10–30% total callbacks** on this div-heavy corpus; **0–3% text reduction or a small increase** for fast paths. Not representative of programs without i64 division. | Shared IR/isel divmod pairing, generated helper in VFS; keep ABI unchanged initially. Pairing/native divmod also benefits em64t, where division is hardware and the absolute gain is smaller. | High: signed MIN/-1 and zero #DE, evaluation order, overflow/remainder semantics, helper sret, interrupt contract. Separate helper-algorithm changes from allocator commits. |
| 5. Immediate/address folding | Repeated constants and multiply-by-two/add address sequences remain; em64t already has `fold_addresses` at isel:353. Target **2–5% text, 1–4% callbacks** small; the 1,424-byte em64t MOVABS bound is directly measurable, plus removal of ALU constant temporaries. | Isel/MIR peephole and encoder immediate choices. Both backends. Respect legal 16-bit BX/BP/SI/DI forms and DS/SS selection. | Low/medium for flag-preserving moves; medium for flag-writing arithmetic folding, displacement overflow and segment/address-width rules. |
| 6. General constant propagation and CSE | Shared pass list lacks SCCP/GVN; repeated operations in six integer matrices survive, with individual custom matrices around 6 KB versus LLVM's roughly 2–2.6 KB. Target **8–20% total text, 2–8% small callbacks**, much more static gain than dynamic on this division-heavy mix. | Shared IR SCCP/algebraic simplification and local CSE first, then dominance-aware GVN. Both backends; useful before MIR allocation. | Medium/high: narrow wrap semantics, divide/trap behavior, pointer provenance, volatility, side effects and memory invalidation. Never merge volatile reads. |
| 7. Redundant MIR moves/addresses and cleanup | 56 identical small-model register moves occupy 112 bytes and execute 375 times. Existing shared move cleanup at regalloc:1973 handles COPY/MOV64rr, leaving narrow self-moves; adjacent duplicate LEAs survive too. Narrow self-moves alone: **0.15% text / 0.11% callbacks**. Broader cleanup might reach **0.5–2% text / 0.5–2% callbacks**. | Shared MIR peephole after allocation, distinct from alias-aware IR load/store work. Both; never erase em64t `mov eax,eax` without proving upper bits irrelevant. | Low for proven full-value self-copies; medium for extensions, address reuse and dead flag-producing instructions. |
| 8. LICM and strength reduction | Slice pointer/length and global address LEAs remain inside loops; small u16 indexing repeatedly multiplies by two. Hoist immutable addresses and use legal pointer induction where profitable. Target **1–4% text, 2–5% small callbacks**, larger routine-local gains. | Shared IR loop analysis/LICM and induction simplification; i8086 isel strength reduction informed by register pressure. Both. | High for memory LICM: aliasing, volatile loads, loops with zero iterations and trapping operations. Medium for wraparound induction, 0xFFFF boundaries and based/far offsets. |
| 9. Branch layout, fusion and relaxation | Small already removes fallthrough jumps and fuses comparisons. 244 currently near branches fit conservative short ranges: **389 bytes (0.52%)**, **zero callback reduction** from shortening alone. Em64t has 149 removable next-instruction jumps and 940 conservatively shortenable branches; shortening bound 3,194 bytes (4.65%), overlapping jump deletion. Direct branch/short-circuit simplification can reduce more. | MIR block layout/fusion; encoder iterative branch relaxation. Both, with a bigger immediate gap on em64t. | Medium: fallthrough CFG, labels/relocations, jump tables, shrinking to a fixed point and flags. Do not speculate side-effecting RHSs. |
| 10. Frame omission and fastestcall | Frame/stack operations execute 4,496 times (1.26%) on small; calls 364 (0.10%), versus spills 37.6%. Current four-register internal ABI, aggregate/sret copies and conservative call clobbers do cost work; frame omission/call specialization has a modest corpus-wide direct bound. Target **1–4% text / 0.5–2% callbacks** before secondary allocation effects. Fastestcall is the next separate roadmap phase. | ABI/frame layout with whole-program SCC analysis and address-taken/C ABI exclusions. Em64t may benefit, with its different ABI constraints. | High for fastestcall: recursion, interop, indirect/address-taken functions, preserved registers, segments, unwind/debugging. Do not fold into early peephole changes. |

Vectorization/unrolling is lower priority for i8086: LLVM's FNV result demonstrates substantial instruction savings but increased text. For em64t it is a later separate opportunity after scalar IR and selection gaps shrink. Strict `-farch 8086` remains after fastestcall; these measurements use the existing i386-capable real-mode backend.

## Verification proposal

Existing hand-written execution fixtures are a strong starting set, but **not enough** to approve arbitrary allocator, width, memory or ABI optimizations alone.

For each approved optimization, use one isolated commit and a targeted execution regression for the changed invariant. During iteration run the affected representative shape. Before proposing adoption, run the i8086 differential list at O0/O2 in all three models, C/internal ABI matrix with NASM interop, far/based and memory/i64/#DE QEMU matrices (including above-1-MiB unreal cases, 0xFFFF wrapping, segment caches, overlapping REP copies and ES preservation), and em64t execution fixtures on custom and LLVM for ELF/COFF. Keep the Linux/Windows cross-backend ABI checks for shared allocator or ABI changes. Use KVM checks where the existing mandatory matrix requires them; TCG alone does not enforce all segment limits. Follow the agreed one end-of-batch gate, rather than rerunning it after every byte change.

Add focused coverage before the relevant step:

- Width narrowing: poison unused register halves, high u16 values above 255, all integer widths/signedness, call preservation of full ESI/EDI, narrow values sharing a spilled register/pair, parallel copy cycles and phi swaps; object and reassembled-listing execution must agree.
- Allocation: randomized live-range pressure with loops/diamonds, constants live across calls, CL shifts, DIV fixed registers, REP implicit operands, pointer addressing masks and overlapping narrow/wide register families. Check results and sentinels, not just exit-only arithmetic sums.
- IR memory work: escaped locals, overlapping pointers/aggregates, volatile/MMIO reads/writes, atomics on em64t, zero-trip loops, conditional traps and far/based qualifiers/segments. Verify ordered side effects as well as returned values.
- Division: boundary tables plus deterministic random dividends/divisors, signedness, quotient/remainder identities, zero and MIN/-1 #DE, helper interop and pair reuse with intervening side effects.
- Layout/fusion: branch-distance boundaries, sparse/dense switch and jump-table relocation cases, fallthrough edges and flag-dependent consumers.

A **seeded randomized differential generator** would materially improve confidence: generate bounded integer/pointer/array programs with defined DC semantics, compare LLVM/native em64t against custom em64t and i8086 in all models, emit an observable memory digest and side-effect trace, record seeds and automatically reduce failures. Exclude undefined physical addresses and deliberately trapping cases from random success tests; test traps separately. Include model-aware near-pointer wrapping and segment cases with explicit reference behavior instead of treating LLVM's flat pointers as the oracle. Establish a nightly/manual larger seed budget and a small fixed-seed commit gate. This is proposed coverage, not implemented here.

Output change review: retain before/after objects, disassembly, MIR slot tables, per-function/category byte and callback deltas, frame deltas and execution results. Explain every changed function by the intended transformation; investigate unrelated changes and correctness failures. Keep exact-output tests for unaffected structural invariants and update changed expected output only after execution evidence supports it. Capture the proposed replacement byte baseline twice from distinct source directories, verify reproducibility and preserve the previous approved baseline. **Submit the report and candidate baseline for explicit user approval after each optimization step; adopt only after approval.** None is adopted by this measurement commit.

## Pre-existing assembly-printer defect

Custom em64t's MOVZX64rm16 listing path prints every memory source with `RegWidth::Bits8` (`compiler/source/backend/em64t/assembler.cppm:456`). The encoder emits a correct word load. A sorting reproduction with values multiplied by 257 exits **0** from the direct object but **1** after NASM assembles `-S` output. Evidence is retained in `/home/admin/.cache/dcc-codegen-measure/printer-probe/`. This should be a separate correctness fix with high-u16 round-trip execution coverage before assembly listings can be trusted as byte-baseline inputs. It was not fixed because this task is measurement and analysis only. Object-based measurement is unaffected.

The report and proposed order now need approval. No optimization work has started and nothing was pushed.
