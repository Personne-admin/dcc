# Compiler benchmarks

Run from the repository root:

```sh
make benchmark ENABLE_LLVM=1 BUILD_TYPE=relwithdebinfo \
  BENCH_ARGS='--compiler-build relwithdebinfo --allow-broken --cpu 2'
make test-benchmark
```

Changing `BUILD_TYPE` does not invalidate existing objects in this repository. For an optimized compiler baseline, first run `make -B driver ENABLE_LLVM=1 BUILD_TYPE=relwithdebinfo -j4`. Benchmark results record the compiler binary hash, declared build type, git commit and dirty state, archive hashes, corpus hash, CPU, affinity, tool versions, and collection options. Do not compare debug and optimized compiler build times as if they were a pass effect.

`mk/benchmark.py` defaults to LLVM and em64t at O0, O1, O2, and Os. `--configs llvm:O0,llvm:O1` selects a subset; adding another driver-supported level uses the same `backend:level` notation. `--filter` selects benchmark names by regular expression. `--output` chooses the JSON file and sibling Markdown summary; binaries, logs, and generated compile-time modules go in a sibling artifact directory. Run serially on an otherwise idle machine. The runner never starts parallel compiler or benchmark processes. CPU affinity is inherited by child processes.

Every executable must exit zero with empty stdout, after checking its results internally. The runner validates before collecting any timings and validates every subsequently timed execution and every recompiled executable. A wrong exit, signal, timeout, or unexpected stdout marks the configuration broken and discards its compile/runtime samples. Failed builds are separate from broken executions. `--allow-broken` permits the command to finish successfully with explicitly reported broken configurations; it never converts failures into timing data. Without it, any failure makes the command exit nonzero. Compiler diagnostic phase measurements on failed configurations are diagnostic information, not accepted performance results.

The default is three warm compile/link samples and nine process runtime samples, reporting all samples, median, median absolute deviation (MAD), minimum, and maximum. Each runtime workload performs thousands of operations internally. Runtime includes launch, library initialization, and validation; this is especially important for the compile-focused and correctness-only cases, whose runtime is not an optimization signal. Compilation includes dcc object generation and a separately timed link. Filesystem caches are warm. There is no claim of a cold-build measurement. Phase measurements come from a separate instrumented compile, so instrumentation does not affect the accepted wall-time samples. Very small phase times need repetition before drawing conclusions.

## What is measured

- `DCC_BENCH_STATS=1` enables stderr records: parse, frontend, IR lowering, backend, the whole dcc pipeline (including cloning), and each pass's accumulated execution time. Frontend includes parsing, sema, and import management. `sema_imports_other` subtracts parsing; it is not a pure sema timer. Backend includes dcc and LLVM passes and code generation, but excludes the harness link. Nested timings must not be added together.
- IR counts include each block's instructions and its terminator, excluding declarations and parameters. Per-function sizes and counts of loads, stores, allocas, phis, and conditional branches are recorded before/after the dcc pipeline. O0 has an identity pipeline. These are dcc IR counts, not optimized LLVM IR counts.
- Small calls target a defined function with 1–20 IR instructions. External/indirect targets without a body cannot qualify. The report includes counts before and after the pipeline. `naive_inline_added_ir` is the sum of callee size minus one per qualifying site: the gross cloned body cost before return/argument cleanup or dead function removal. It is not an expected speedup or a prediction of final size. Inlining could remove at most the qualifying static call sites in this estimate.
- File, object, and text sizes are bytes. `machine_instructions` counts statically disassembled instructions in executable sections, including linked library and harness routines, with GNU objdump. It is not an executed instruction count.
- `--perf /path/to/perf` collects hardware cycles, executed instructions, branches, branch misses, cache references, and cache misses in a separate validated run. Unsupported counters stay explicitly unavailable in the raw CSV; they must not be read as zero. Counter collection is separate from runtime samples.
- `--profile --valgrind /path/to/valgrind` collects Callgrind function attribution, dynamic instructions, and simulated cache/branch events. `VALGRIND_LIB` can point at a local installation. Simulated misses are not hardware misses. `--profile-existing --output build/benchmarks/baseline.json --configs llvm:O0,llvm:O1 --filter 'map|numeric' --valgrind ...` profiles already validated saved binaries without recompilation or another timing sweep. Callgrind failures do not supply profiles.

Compare with a saved run using `--baseline path/to/baseline.json`; the Markdown output reports current/previous median ratios and warns when the corpus changed. Compare correctness status and hashes before reading ratios. Keep the JSON and relevant profile annotations with a findings document, while retaining large binaries and compiler logs under `build/` locally.

## Controlled comparisons and linking

`llvm:O1:no-ir` sets `DCC_BENCH_SKIP_IR_PASSES=1`, bypassing dcc's pipeline while retaining LLVM's normal O1 pipeline. This opt-in measurement control is also available on em64t. No optimization pass is added. LLVM O0/O1 alone cannot isolate mem2reg/dce/simplifycfg because LLVM also changes its own pipeline.

By default, the runner links the standard per-backend Linux archives, which the normal makefile builds at O0, independent of application optimization. `--library-opt O2` builds private archives from the same sources in the run's artifact directory. This allows a separate measurement of library compilation policy without changing installed archives. Its one-time archive build cost is excluded from per-application compile timings. Private archives are experimental configurations and require the same correctness gate.

The harness uses dcc `-c`, then `ld.lld` with the normal libdcext startup code. LLVM's optimized generated code can reference `memset`, `memcpy`, or `memmove`, absent from the normal freestanding archive. `runtime.c` supplies those three symbols identically to both backends. It is compiled with clang O2, freestanding/no-builtin and volatile byte accesses to prevent recursive intrinsic generation. This is an explicit benchmark link dependency, not a compiler/runtime optimization. Calls to these routines must remain visible in profiles. Results therefore describe this link contract, not an unmodified `dcc -o` invocation.

## Corpus

Array, map, allocator, sort-with-allocator, formatter, result-wrapper, and short UTF-8 checks are adapted from the existing libdcext container/module integration programs, with repeated workloads. Sources deliberately contain their checks and no explanatory comments; the provenance and measurement contract live here.

- Array: growth, push/pop, resize, iteration/access, ownership, and allocator interactions.
- Map: insertion, replacement, lookup, removal, iteration, and explicit 16/128, 48/128, and 80/128 occupancy cases. These use deterministic integer keys; they are not a large working-set or adversarial-collision study.
- Sort: 256-element sorted, reverse, permuted, and duplicate-heavy arrays; allocator/comparator paths; the same 128-element algorithm over i32, u32, i64, u64, f32, and f64. Generic-sort failure codes 1–6 identify that type order.
- Wrappers: libdcext Result/Status/Optional forwarding APIs, with runtime-varying success payloads; array/map workloads include len and pointer access wrappers. Current libdcext handles are public fields, so no invented handle-accessor API is benchmarked. Wrapper execution is batched to bound O0 loop-local stack allocation.
- Question-mark chains: four fallible steps, successful and error paths, validated values.
- Formatting: boolean, signed integer, floating-point precision, and string formatting, checked byte-for-byte.
- Allocators: fixed-buffer and arena allocate/free/reset, zeroing, creation, duplication, resize, and allocation failure.
- Numeric: exp/log/sqrt/sin/cos over 100 values, finite checks, identities, and independent aggregate reference values computed with Python's math module.
- UTF-8: valid/invalid narrow-byte sequences and slice operations, plus a multilingual stream with encode/decode round trips, an independent codepoint checksum/count, and string search.
- Correctness probes: existing narrow-ALU, narrow-shift, and negative-switch execution cases. Their sub-millisecond runtime is not used for recommendations.
- `mk/benchmark_corpus.py` generates large single modules (64/256/1024 functions), diamond CFGs (16/64/256 conditions), distinct-type generic instantiations (16/64/256), and deep import chains (8/32/128). Deep imports use generic forwarding definitions so all code is emitted in the root object. These stress compilation rather than claim to model application runtime. Expected results are generated analytically and checked by the executable.

Workloads are deterministic and mostly fit in cache. They cannot establish large-data DRAM bandwidth behavior or production input distributions. A profile of validation work is still part of the measured workload; avoid presenting its share as pure application overhead. If an optimization folds an entire deterministic computation away, report that as such rather than interpreting a launch-time floor as useful runtime speed.

A later middle-end folder must respect runtime mutation and control flow. The prior sema loop-condition invalidation defect is a specific warning: a variable written in a loop cannot be treated as a loop-invariant constant merely because its initial value is known.
