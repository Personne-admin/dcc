# C-string UFCS formatting

`print.dc` keeps its C-string formatter private. Generic calls through `println`,
`print`, `std::fmt::format`, and `std::fmt::format_value` can still use it because
it is declared directly in an active instantiation context.

Build from the repository root after `make`:

```sh
build/bin/dcc -Idocs/examples/ufcs-format -Ilibdcext -flibdcext -c docs/examples/ufcs-format/print.dc -o /tmp/ufcs-print.o
build/bin/dcc -Idocs/examples/ufcs-format -Ilibdcext -flibdcext -c docs/examples/ufcs-format/main.dc -o /tmp/ufcs-main.o
build/bin/dcc -flibdcext /tmp/ufcs-main.o /tmp/ufcs-print.o -o /tmp/ufcs-format
/tmp/ufcs-format
```

The runnable example uses the hosted runtime's slice-based `main` signature.
The original `i32 main(i32 argc, char** argv)` also passes compilation with the
corrected formatter, but the hosted startup does not call that signature.

The formatter counts bytes without advancing `str` and returns
`Status(FmtError)`. It delegates the resulting byte slice to the existing public
`format_value` overload, which applies padding and alignment and propagates
write errors. No new standard-library API is needed. There is no duplicate
formatter in `main`.

## Compiler diagnosis

Before the compiler fix, the original call chain reports:

```text
libdcext/std/fmt.dc:465:9: error: type is not formattable: add format(const T*, const Writer*, const Options*)
```

Temporary tracing inside overload diagnostics revealed the suppressed failure:

```text
no matching UFCS function for `format` on receiver type `char*`
format(const Writer*, []const u8, T...): receiver type mismatch: expected `const Writer*`, found `char*`
```

`commit_specialization` retained the outermost module and the current generic's
definition module, but replaced the intervening definition context on every
nested call. `resolve_ufcs` therefore never considered `print::format`. Retaining
the active specialization modules restores their direct declarations without
adding their imports or unrelated private associated functions. All candidates
still undergo the existing overload ranking and declaration deduplication.

After that repair, the formatter became a candidate but the probe rejected its
first explicit argument. `analyze_compiles` assumed parameter types were already
resolved. In deferred generic branches, `const Writer*` and `const Options*`
were not resolved. Resolving these types before defining the probe locals makes
the probe agree with ordinary calls.

The minimal regressions cover the following behavior before the fixes:

| Call context | Receiver | Private hook | Public hook |
| --- | --- | --- | --- |
| B's ordinary function calls A's generic | `char*`, `const char*`, B's struct | Pass | Pass |
| B's generic forwards to A's generic | `char*`, `const char*` | Fail | Fail |
| B's generic forwards to A's generic | B's struct | Fail | Pass through associated lookup |
| An additional generic forwarding level | Same receivers | Same results | Same results |

Both plain UFCS calls and `compiles` probes exhibit the missing-context failure.
All positive variants pass after the fixes. Additional regressions cover
unrelated-import isolation, context restoration, missing hooks, ambiguous
hooks, deferred probe parameter types, and the invalid `void` return type.
The separately compiled executable regression runs on LLVM and em64t and checks
C-string contents, empty strings, padding, mutable and const pointers, and
write-error propagation. `make -j8 test` passes all 31 suites, including all
1,672 case-dispatcher checks.

The standard-library probe still checks `t.format(q, o);`, whereas the real body
uses `?`. A `void` formatter therefore passes the callability probe and fails
with `type void is not Unwrappable: no visible .is_ok() method` in the real body.
This is a difference between the two expressions, not a lookup discrepancy.
A stricter library contract could probe `t.format(q, o) ?;` or require an exact
`Status(FmtError)` result; this change leaves that API policy untouched.

Two distinct, equally viable private formatters in `main` and `print` are
ambiguous under the documented lookup rules. Only repeated discovery of the
same declaration is deduplicated. Keeping only `print::format` removes that
ambiguity. Inside the standard-library `compiles` probe, ambiguity makes the
probe false and produces the formattability `compile_error`; an ordinary call
reports `ambiguous UFCS call`.

The existing specialization registry is keyed by declaration and template
arguments, not lookup context. The specification does not define how different
callers providing different hooks for the same specialization should interact;
this patch preserves the existing specialization identity and caching policy.
