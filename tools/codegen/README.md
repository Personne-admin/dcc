# Code generation measurement

Run from any directory, after building dcc:

```sh
CXX=clang++ CC=clang AR=ar make -j4 DCC_WRAPPER="timeout 60"
systemd-inhibit --what=sleep --why="dcc measurements" \
  python3 tools/codegen/measure.py --output "$HOME/.cache/dcc-codegen-measure/new-run" --jobs 4
python3 tools/codegen/check.py "$HOME/.cache/dcc-codegen-measure/new-run"
```

Dependencies: Python 3, clang, pkg-config and glib development headers, NASM, GNU readelf/objdump/ld, QEMU system-i386 and x86_64 user mode with plugins, and the installed `qemu-plugin.h`. The plugin uses QEMU API 7 (validated with QEMU 11.1.2). Build against the headers belonging to the QEMU executable being used. No perf installation is required. The script uses subprocess timeouts for every compiler invocation and guest execution.

`--only SUBSTRING` selects corpus names for iteration; `--configs small unreal unreal32 em64t llvm` selects configurations. Defaults measure the complete current `tests/i8086/differential.list` and five multi-module realistic workloads. Every DC compilation uses `-O2 -fwhole-program`. Small/unreal/unreal32 use the custom i8086 backend; em64t uses custom x86_64-elf; llvm uses LLVM x86_64-elf. Each execution must match the fixture's expected exit code. Both x86-64 builds also execute natively before QEMU measurement. Outputs are linked without section garbage collection to account for emitted functions.

The hashing routine is extracted from `libdcext/std/hash.dc` (`fnv1a_32`). Insertion sort specializes the algorithm in `libdcext/std/sort.dc` to u16 and direct comparisons, avoiding the currently unsupported i8086 indirect comparator call. Memory/string, token scanning and integer workloads are small freestanding programs with runtime-loaded inputs and checked results. The 66 differential fixtures are reused without edits. The current corpus does not benchmark extended-memory far/based accesses or arbitrary workloads outside this list.

`measurements.json` retains provenance, commands, every decoded instruction, category and per-address execution count; `functions.csv` contains one row per function; `summary.md` contains totals and largest functions. Objects, executable images, listings, MIR, source extraction, linker maps and raw callback counts remain beside them. Measurement failures are reported and cause a nonzero script exit; they are never skipped.

## Accounting

- Static code bytes are ELF function-symbol sizes, excluding gaps between functions. Instruction boundaries come from object/executable disassembly, never the assembly printer. i8086 function addresses come from the linker map and sizes from the ELF32 object. Internal alignment inside a function is included.
- Seven exclusive instruction categories: spill stores, spill reloads, register copies, stack/frame setup and teardown (including argument stack adjustment), branches (including returns), calls, and everything else. Register extension operations, immediate materialization, ordinary loads/stores, arithmetic, NOPs and string instructions belong to everything else. Push/pop instructions are counted as stack/frame operations.
- Custom spills use the final MIR slot offset, size and `is_spill` metadata. Ordinary local slots are separate. LLVM spill locations use its verbose `Spill`/`Reload`/`Folded Reload` annotations; folded memory uses of a spill location count as reloads. LLVM locations are distinct physical `(base, displacement)` spill addresses, not the number of abstract pre-coloring frame objects. The environment variable `DCC_MEASURE_ASM_VERBOSE=1` enables annotations only for assembly emission. Default output stays unchanged.
- Custom frame size is the backend's final reserved area, excluding frame-pointer and callee-save pushes. LLVM reports explicit stack allocation plus caller-save alignment pushes, or its red-zone extent, also excluding callee-save pushes. Total frame bytes and spill locations are sums across emitted functions, not peak call-stack usage.
- Prefix66/prefix67 counts are **bytes**, overlapping the instruction categories. Only decoded leading prefix bytes count: immediate/displacement bytes with the same numeric values do not. Executed prefix and instruction-byte totals weight each instruction's encoded bytes by its callbacks; they are not a hardware instruction-fetch or cycle measurement.
- The QEMU plugin attaches an execution callback to each measured instruction address and uses atomic counters. BIOS, loader, startup and assertion failure stubs are outside the measured function address set. The x86-64 runtime functions actually referenced by compiler-emitted libcalls are included; unused runtime functions are excluded. A fixed clang-built freestanding runtime supplies strlen/memset/memcpy/memmove/memcmp; no host libc or dynamic loader is counted.
- Callback counts describe instructions reached in these successful deterministic runs, not elapsed time or a hardware micro-op count. Calibration checks both 16-bit and 64-bit loops with five executions of the body and branch, and REP STOSB copying four bytes: QEMU 11.1.2 reports **one** instruction callback for REP, not four. This does not measure the internal work or cycle cost of REP.

The tools are outside the test suite. `check.py` validates metric accounting, instruction-boundary prefix decoding, retained custom slot metadata and plugin calibration; it does not invoke the full gate or change a baseline.
