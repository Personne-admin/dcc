# Compile-time diagnostics and static reflection

This guide shows how the compile-time pieces fit together: evaluating code at
compile time with containers, reporting your own diagnostics, asking the
compiler about types, and explaining why a requirement failed. The rules are
in the language specification: compile-time evaluation and the compile-time
heap (section 14.4), compile-time actions (section 18), static reflection
(section 9.8) and `static for` (section 8.5).

## Code that runs at compile time

Any function whose body the compiler can evaluate runs at compile time when a
constant is needed: a `const` global, a `static if` condition, an array
extent or a compile-time action message. There is no attribute for it.
`Array(T)` and the other containers built on `std::alloc` work there too:
during compile-time evaluation they allocate on the compiler's compile-time
heap, whatever allocator policy they use.

```dc
usize sum_squares(usize n) {
    Array(usize) values = {};
    defer values.deinit();
    for usize i = 1; i <= n; i++ {
        if !values.push(i * i).is_ok() { return 0; }
    }
    usize total = 0;
    for usize i = 0; i < values.items().len; i++ { total += values.items()[i]; }
    return total;
}

const usize SQUARES = sum_squares(10); // 385
```

A `[]const T` view of compile-time heap storage, such as a string built at
compile time, becomes constant data. A value that still owns compile-time
storage (an `Array` returned from such a call) cannot be kept at run time and
is an error. `core::is_compile_time()` tells library code which situation it
is in; generated code sees `false` and keeps only the run-time branch.

## Reporting diagnostics

`core::compile_error`, `core::compile_warning` and `core::compile_note` report
a message. When the message is a constant where the call is written, the
action happens during semantic analysis. When it is computed, the action
happens when compile-time evaluation reaches the call, with the evaluated
message, and the function containing it becomes compile-time only: every call
to it is evaluated at compile time and it generates no code.

`std::fmt` builds on that with formatted messages, using the ordinary
formatter (including your own `format` hooks):

```dc
import std::fmt;

void limit(T)(T value) {
    static if sizeof(T) > 4 {
        std::fmt::compile_error("`{}` is {} bytes, the limit is 4", core::meta::type_name!T(), sizeof(T));
    }
}
```

The error is reported at the call that started the compile-time evaluation.
To point somewhere else, pass a `core::source_location::SourceLocation`,
usually from a defaulted `core::source_location::source_location()` parameter
that captures your caller:

| Function | Reports |
| -------- | ------- |
| `std::fmt::compile_error(fmt, args...)`, `compile_warning`, `compile_note` | at the outermost call of the evaluation |
| `std::fmt::compile_error_at(at, fmt, args...)`, `compile_warning_at`, `compile_note_at` | at `at` |
| `std::fmt::note_at(at, fmt, args...)` | returns a `core::Note` |
| `std::fmt::compile_error_with(at, notes, fmt, args...)` | an error at `at` with the notes attached |

The unformatted `core::compile_error_at`, `compile_warning_at`,
`compile_note_at` and `compile_error_with` take a ready message.

## Asking about types

`core::meta` answers questions about types as constants: names, kinds,
integer widths, array lengths, generic arguments, struct and union fields,
enum variants, function parameters, attributes and declaration locations.
`static for i in 0..N` repeats code once per index with `i` constant, which is
how the per-index queries cover a whole type:

```dc
import core::meta;

Status(Error) show(T)(T* value) {
    print("{} {{", core::meta::type_name!T(core::meta::NameStyle::Plain))?;
    static for i in 0..core::meta::field_count!T() {
        print(" {} = {}", core::meta::field_name!T(i), *core::meta::field!T(value, i))?;
    }
    println(" }}")?;
    return Status::Ok;
}

[] const char variant_label(T)(T value) {
    static for i in 0..core::meta::variant_count!T() {
        if value == core::meta::variant!T(i) {
            return core::meta::variant_name!T(i);
        }
    }
    return "?";
}
```

## Explaining failed requirements

`compiles (...) { ... }` and concept applications such as `Printable(T)` are
`bool` constants. Ask for a `core::meta::Check` instead and you get the
failing requirement, the first error under it and both locations, including
errors inside templates the requirement instantiated:

```dc
void require_hook(T)() {
    const core::meta::Check hook = compiles (T t, std::fmt::CountingWriter* w, const std::fmt::Options* o) {
        t.format(w, o);
    };
    static if !hook.ok {
        core::Note[1] notes = { std::fmt::note_at(hook.first.location, "{}", hook.first.message) };
        std::fmt::compile_error_with(core::source_location::source_location(), notes[0..1],
            "`{}` has no usable format hook", core::meta::type_name!T());
    }
}
```

Nothing is reported unless your code reports it, so a library decides what to
say and where.
