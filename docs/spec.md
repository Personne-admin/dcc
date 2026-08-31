# DCC Language Specification - Draft v0.2

## 1. Primitive Types

| Type                      | Size       | Description                                          |
|---------------------------|------------|------------------------------------------------------|
| `i8`, `i16`, `i32`, `i64` | 1, 2, 4, 8 | Signed two's complement integers                     |
| `u8`, `u16`, `u32`, `u64` | 1, 2, 4, 8 | Unsigned integers                                    |
| `usize`, `isize`          | ptr        | Pointer-sized unsigned/signed integers               |
| `f32`, `f64`              | 4, 8       | IEEE 754 floating point                              |
| `bool`                    | 1          | `true` / `false`                                     |
| `char`                    | 1          | ASCII character                                      |
| `void`                    | 0          | Unit / absence of value                              |
| `null_t`                  | ptr        | Type of `null`, inhabits all pointer/optional types  |

### 1.1 Restricted Integer Types

An integer scalar type can be restricted to a subset of its domain by
appending a `{...}` suffix that enumerates a finite set of values and/or
inclusive ranges:

```dc
using Width   = u8{32, 64};              // finite set
using Percent = u8{0..100};              // inclusive range
using Mixed   = u16{0..10, 100, 200..300}; // set and ranges, mixed
```

Only integer scalar types accept a restriction (`i8`..`i64`, `u8`..`u64`,
`usize`, `isize`). Restricting any other type is an error.

**Where restrictions are legal.** A restriction may appear in any
declaration type position — variable, parameter, return type, struct field,
array element type, and type alias — and as the target of an `as` cast in
three forms:

```dc
using Width = u8{32, 64};

u8{32, 64} a = v as Width;          // 1. named alias
u8{32, 64} b = v as (u8{32, 64});   // 2. parenthesized
u8{32, 64} c = v as u8{32, 64};     // 3. bare (unambiguous position)
```

The bare form is accepted only where the `{` cannot begin a following
block. In `if`/`while`/`match` heads the brace binds to the body block, so
the bare form is unavailable there; the alias or parenthesized form is
required:

```dc
if v as (u8{32, 64}) { ... }   // parenthesized: the {..} is the restriction
if v as u8 { ... }             // the {..} is the block, not a restriction
```

**Normalization.** A restriction is canonicalized before it is interned:
intervals are sorted by lower bound, duplicates collapse, and overlapping
intervals — including those that share an endpoint — are merged. Disjoint
intervals remain separate. Two restrictions that normalize to the same
interval set are the *same type* (pointer-identical after interning), so
`u8{32, 64}` and `u8{64, 32}` name one type.

Each endpoint must be an integer compile-time constant expression; a
non-constant endpoint is an error.

**Representation.** A restricted type has the same size, alignment, and ABI
as its underlying integer. The refinement survives only in the type system
and in mangled names (so overloads and specializations distinguish distinct
restrictions); it never changes storage or code generation.

**Conversions.** There is no implicit conversion into a restricted type
except the contextual typing of an integer literal, which is checked at
compile time:

```dc
u8{32, 64} ok  = 32;   // ok: 32 is a member
u8{32, 64} bad = 16;   // error: 16 is not a member of u8{32, 64}
```

Producing a restricted value from any other integer expression requires an
explicit `as` cast. Converting *out* of a restricted type is implicit in
the widening direction: a restricted type converts to its underlying
integer (and to wider integers), and to a wider restriction that contains
it:

```dc
u8{32, 64} w = ...;
u8         raw  = w;   // restricted -> underlying integer
u32        wide = w;   // restricted -> wider integer
u8{0..100} pct  = w;   // restricted -> wider restriction (contains it)
u8{16, 64} nope = w;   // error: not a superset
```

Arithmetic erases the refinement: `+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`,
`<<`, `>>` on restricted operands produce the underlying integer type, not
a restricted type.

**Casting in is unsafe.** `value as u8{32, 64}` asserts that `value` is a
member. An out-of-domain value is undefined behavior unless
`-frestricted-check` is given, in which case the compiler emits a runtime
membership check that traps through the same `__assert` runtime as
`-fbounds-check`. Constant operands are always checked at compile time,
regardless of the flag.

**Known limitations.** A `@nominal` alias wrapping a restricted type does
not propagate membership checking: `@nominal using CpuBits = u8{0..255};
CpuBits cb = 16;` reports a generic type mismatch rather than a membership
diagnostic. `match` exhaustiveness checks the underlying integer domain,
not the restricted domain, so a `match` over a restricted value is not
flagged as incomplete when it covers only the restricted values.

---

## 2. Composite Types

### 2.1 Structs

No constructors, destructors, or member functions. Data-only aggregates.
Initialization via struct literals. Behavior is attached externally via free
functions and UFCS.

```dc
struct Point {
    f32 x;
    f32 y;
}

// factory pattern: called as Point::new(1.0, 2.0)
public Point new_point(f32 x, f32 y) {
    return Point { x = x, y };
}
using Point::new = new_point;

// UFCS method: called as p.length()
public f64 length(const Point* self) {
    return sqrt((self.x * self.x + self.y * self.y) as f64);
}
```

### 2.2 Unions

Untagged, C-style. Unsafe by nature. Access to the wrong field is undefined behavior.

```dc
union Register {
    u32 full;
    u16[2] halves;
    u8[4] bytes;
}
```

### 2.3 Enums

Two forms: plain enums and tagged enums exist.

```dc
// plain enum: default backing type is i32, overridable
enum Color : u8 {
    Red,        // 0
    Green,      // 1
    Blue = 10,  // explicit
}

// tagged enum: each variant optionally carries data
enum Result(T, E) {
    Ok(T),
    Err(E),
    Cancelled,    // no payload
}

enum MaybeInt {
    Int(i32),
    NoInt,
}

enum Optional(T) {
    Some(T),
    None,
}
```

---

## 3. Pointers & References

Only raw pointers, no references. No `->`, only `.` with automatic dereference
through any number of pointer indirections.

---

## 4. Qualifiers

`const`, `restrict`, `volatile` applied to declarations.

```dc
i32*          // pointer to i32
const i32*    // pointer to const i32
volatile u8*  // pointer to volatile u8
i32* const    // const pointer to mutable i32
```

---

## 5. Arrays, Slices & Flexible Array Members

Three distinct compound types with different syntax positions:

```dc
u8[256] array;       // fixed-size array: 256 bytes, value type, stack-allocable
[]u8 slice;          // slice: fat pointer (ptr + len), does NOT own memory
u8[] fam;            // flexible array member: must be last field in a struct or extern global variable, unsized
```

The grammar pattern:

| Syntax | Meaning          | Size known at     | Owns memory |
|--------|------------------|-------------------|-------------|
| `T[N]` | Fixed array      | Compile time      | Yes         |
| `[]T`  | Slice            | Runtime (fat ptr) | No          |
| `T[]`  | FAM              | Unkown            | No          |

---

## 6. Type Aliases, Concepts & Using

`using` is the multi-tool keyword. Its meaning is determined by what follows it.

### 6.1 Type alias

```dc
using Vec3 = f32[3];
using Callback = void(*)(i32, i32);   // function pointer type
```

#### 6.1.1 Nominal aliases

A plain `using` alias is structural: the alias and its target are the same
type everywhere, interchangeably. Prefixing the declaration with `@nominal`
instead creates a distinct type with the same representation as the target,
but no implicit conversion either way, conversion requires an explicit
`as` cast, and overload resolution treats it as a different type from its
target.

```dc
@nominal using Fd = i32;

Fd make_fd() { return 0 as Fd; } // explicit cast required to produce a Fd

void take_fd(Fd x) {}

void test() {
    Fd fd = 0 as Fd;
    i32 raw = fd as i32; // explicit cast required to go back to i32

    take_fd(raw); // error: no matching call, i32 is not Fd
}
```

### 6.2 Concept definition

```dc
using Printable(T) = compiles(T t) {
    print(t);
};

using Numeric(T) = compiles(T a, T b) {
    a + b;
    a * b;
};
```

### 6.3 Module symbol import

```dc
using Vec3 = math::Vec3;                 // bring math::Vec3 as Vec3
using geom::Point = math::Point;         // bring as geom::Point (custom namespace)
using Point = math::geom::Point;         // shorthand
```

### 6.4 Re-export into importer's scope

```dc
// In module `graphics`:

// Makes `math::Vec3` appear as `Vec3` in anyone who imports `graphics`
using public Vec3 = math::Vec3;

// Makes it appear as `graphics::Vec3` in the importer (normal re-export) 
public using Vec3 = math::Vec3;
```

The distinction:

| Syntax                    | Effect                                            |
|---------------------------|---------------------------------------------------|
| `using X = Y;`            | Private alias, local scope only                   |
| `public using X = Y;`     | Alias is importable: importers see `ourmod::X`    |
| `using public X = Y;`     | Direct injection: importers see `X` unqualified   |

### 6.5 Value alias

A value alias binds a name to a compile-time constant value:

```dc
using u64 A = 1;
using u64[4] B = { 1, 2, 3, 4 };
using Foo* P = null;
using ns::Foo X = some_constant_expression;
```

The declared type comes first, followed by the binding name and an `=`
initializer. The initializer must be a compile-time constant expression and is
checked and converted against the declared type, in the same way as a `const`
object initializer. Value aliases may reference other value aliases (including
aggregate elements) and compile-time constants, and they may be referenced from
normal runtime expressions, where they evaluate to their constant value.

A value alias is not an object. It has no storage, no address, and no
writable identity:

- `&A` is an error: the value alias has no storage or address.
- Assignment and increment/decrement of a value alias are errors.
- A value alias declaration never creates a global, a `.rodata`/`.data`
  entry, or any object symbol on its own.

To materialize an actual object holding the value, declare a `const` object:

```dc
using u64[4] Values = { 1, 2, 3, 4 };
const u64[4] stored = Values;   // `stored` is a real object
```

This is the same distinction as between a type alias and the type it names:
a value alias is a name for a value, whereas `const` declares a named object
that happens to be immutable. Visibility forms work as for type aliases:

| Syntax                          | Effect                                         |
|---------------------------------|------------------------------------------------|
| `using u64 A = 1;`              | Private value alias, local scope only          |
| `public using u64 A = 1;`       | Importable: importers see `ourmod::A`          |
| `using public u64 A = 1;`       | Direct injection: importers see `A` unqualified |

---

## 7. Module System

### 7.1 Declaration

```dc
module graphics;              // every file begins with a module declaration

import math;                  // import module, compiler finds file `math.dc`, folders are ignored.
import std::io;               // sub-module import
```

Module and import paths must match a walkable path by the compiler from any of the configured import paths.
The folder in which the currently-being-compiled file is located is automatically added to the import path.

If file is located at `/std/math/sin.dc` relative to an import path, it can be declared as either `module sin;` or `module std::math::sin;`. (the latter is advised)
In both cases, the importer must write the full qualified path; `import std::math::sin;`.
The name of a module may collide with a folder name along a qualified path; `/std/math.dc` and `/std/math/sin.dc` are both allowed, importable as `import std::math;` and `import std::math::sin;` respectively.

#### 7.1.1 Path Resolution and Common Parent Stripping

When resolving the namespace of an imported module, the compiler automatically
calculates the common prefix between the current module's path and the target
module's path and strips that prefix from the visibility namespace. This
prevents redundant scoping and allows for cleaner access to deep module
hierarchies.

- If the current module is `a` at `project/f1/f2/a.dc` (Namespace: `f1::f2::a`).
- And the target module is `b` at `project/f1/f2/f3/b.dc` (Namespace: `f1::f2::f3::b`).
- An import statement import `f1::f2::f3::b;` (or shorthand `import f3::b;`) inside `a.dc` will result in `b` being accessible as `f3::b`.

### 7.2 Circular imports

Modules can import each other.

### 7.3 Visibility

Functions, structs, unions, enums, type alias and other top-level declarations are private by default. The `public` keyword as the first keyword of a declaration must be used to allow the produced name to be imported and used.

```dc
public struct Point {
    f32 x;
    f32 y;
}

public f32 distance(Point a, Point b) { ... }

// private
f32 helper(f32 x) { ... }
```

---

## 8. Templates

### 8.1 Function templates

```dc
// template type parameter
T max(T)(T a, T b) {
    return if a > b { a } else { b };
}

const u8 COM_DATA = 0x0;

// template value parameter
void serial_write(u8 COM_BASE)(u8 byte) {
    volatile u8* port = COM_PORT + COM_DATA as u64 as volatile u8*;
    *port = byte;
}

// multiple parameters
void copy(T, u64 N)(T* dst, const T* src) {
    for u64 i = 0; i < N; i++ {
        dst[i] = src[i];
    }
}
```

**Call syntax:**

For single parameter functions automatic template type deduction and the following syntax is allowed:

```dc
T max(T)(T a, T b) {
    return if a > b { a } else { b };
}

void switch_ctime(u64 a)() {
    static if a == 2 {
        ...
    } else {
        ...
    }
}

max!i32(32, 33);
max(32, 33); // due to the inability of the compiler to guess the literal type, it is defaulted to i32.

i32 value = 32;
max(value, 33);
max!u64(value as u64, 33);

switch_ctime!1();
switch_ctime!2();
```

For multiple parameter template functions, an explicit `()` around the template parameters is required:

```dc
void copy(T, u64 N)(T* dst, const T* src) {
    for u64 i = 0; i < N; i++ {
        dst[i] = src[i];
    }
}

u8* dst1 = ...;
const u8* src1 = ...;
copy!(u8, 23)(dst1, src1);
```

### 8.2 Struct templates

```dc
struct Pair(T, U) {
    T first;
    U second;
}

Pair(i32, f64) p = { first = 1, second = 2.0 }; // type of rhs is implicitly guessed from the right hand side.
Pair(u8, bool) p2 = { 0xFF, false };
```

### 8.3 Constrained templates

```dc
using Addable(T) = compiles(T a, T b) { a + b; };

T sum(T)([]const T items) if Addable(T) {
    T acc = items[0];
    for u64 i = 1; i < items.len; i++ {
        acc += items[i];
    }

    return acc;
}
```

Struct and enum templates can be constrained the same way; the constraint
is checked at every instantiation site, including nested ones:

```dc
struct Slot(T) if Addable(T) {
    T value;
}

Slot(i32) good; // OK, i32 satisfies Addable
Slot(NoAdd) bad; // error: template constraint not satisfied for `Slot`
```

### 8.4 `static if` and `static match`

```dc
void process(T)(T value) {
    static if compiles(T t) { t.serialize(); } {
        value.serialize();
    } else static if T == u8 {
        raw_write(value);
    } else {
        static match T {
            u16 => raw_write16(value);
            _ => compile_error("T must be serializable, u8 or u16");
        }
    }
}

void execute(u8 command)() {
    static if command == 1 {
        ...
    } else if command == 2 {
        ...
    } else {
        static match command {
            3..=10 => ...,
            11..13 => ...,
            13 => ...,
            _ => ...
        }
    }
}
```

### 8.5 Variadic templates

A trailing `T...` in a template parameter list declares `T` as a type pack
rather than a single type. A function parameter declared with a pack type
becomes a pack parameter: it expands to one parameter per type in the pack
at instantiation.

```dc
u64 count(T...)(T x) {
    return sizeof...(T);
}

count!(i32)(42); // 1
count!(i32, i64)(0, 1); // 2
```

Pack parameters can be indexed with a compile-time constant, and iterated
with `static for`:

```dc
void pick(T...)(T args) {
    take_i32(args.0);
    take_f64(args.1);
}

void each(T...)(T args) {
    static for item in args {
        process(item);
    }
}
```

A pack parameter can be forwarded to another variadic function by
re-expanding it with `...`:

```dc
void inner(T...)(T x) { target(x...); }
void outer(T...)(T x) { inner(x...); }   // forwards outer's pack to inner

outer!(i32, i64)(0, 1);
```

Variadic template parameters are allowed on function templates and concept
definitions (`using Foo(T...) = compiles(T x) { ... };`) but explicitly
not on struct/enum declarations.

---

## 9. Expressions

### 9.1 Block expressions

Braces form a block expression. The last expression (without `;`) is the block's value. Pasted from rust.

```dc
i32 x = {
    i32 a = compute();
    i32 b = transform(a);
    a + b    // no semicolon: this is the block's value
};

// if-else is an expression
i32 y = if cond { 1 } else { 2 };

// match is an expression
i32 z = match opt {
    Optional::Some(v) => v,
    Optional::None => 0,
};
```

### 9.2 Casts

```dc
f64 x = 3.14;
i32 y = x as i32;        // truncating cast
u8 b = 256 as u8;        // error
u8* p = addr as u8*;     // integer to pointer
```

### 9.3 Literals & type inference

No implicit conversions, so literals need to be smart:

```dc
i32 x = 42;        // 42 is i32
u8 y = 42;         // 42 is u8
u8 z = 256;        // error
f64 w = 3.14;      // 3.14 is f64
f32 v = 3.14;      // 3.14 is f32

// ambiguous context:
T function(T)(T a) { ... }

i32 a = function(32); // inferred to i32
u8 b = function(32); // inferred to u8
function(32); // value discarded, guessed to i32.
```

Character and string literals come in an 8-bit and a UTF-16 form. A plain
literal is `char`/`[]const char`; a `u`-prefixed literal is `u16`/`[]const u16`,
with the same escape sequences (`\n`, `\0`, `\xNN`, `\uNNNN`) available in both:

```dc
const char ch       = 'H';
[]const char greeting = "hello";

const u16 wch        = u'Ω'; // Ω, UTF-16 code unit
[]const u16 wgreeting = u"hi";
```

### 9.4 Compile-time size/alignment queries

`sizeof`, `alignof`, and `offsetof` are builtins evaluated at compile time;
all three produce a `usize`.

```dc
struct S {
    u8 a;
    i64 b;
}

usize sz = sizeof(i32);
usize al = alignof(S);
usize off = offsetof(S, b);
```

Inside a variadic template, `sizeof...(T)` instead queries the number of
elements in a type pack `T` (see §8.5), using `sizeof...` on a name that
isn't a pack is an error.

---

### 9.5 The Unwrap/Propagate Operator (`?`)

The postfix `?` operator unwraps a success value from a container type and
propagates the error value by returning early from the enclosing function.

**Syntax.** `expr?` is a postfix operator at the same precedence as
postfix `++` / `--`. It binds tighter than binary operators and the `as`
cast, but looser than the atomic primary expression:

```dc
call()?;               // ok: ? on a call result
object.field?;          // ok: ? on a field access
items[index]?;          // ok: ? on an index expression
val as T?;              // possible error: `as` binds the type first, then `?` on the cast
```

The operand is evaluated **once**. The result of `?` is always a prvalue.

**Where valid.** `?` is only permitted inside a function body
(including generic functions and UFCS methods). Using `?` outside a
function is a compile-time error for now.

**Protocol via UFCS.** The compiler resolves three methods on the
operand type via ordinary UFCS lookup (§11):

1. `.is_ok()` — must return a `bool`-compatible type.
2. `.unwrap()` — returns the **success payload**. The return type is the
   type of the whole `?` expression.
3. `.unwrap_err()` — returns the **error payload**. Its return type must
   be compatible with the enclosing function's return type.

All three methods must be visible in the scope where `?` appears. Template
instantiation re-resolves the methods for each instantiated type; generic
`is_ok` / `unwrap` / `unwrap_err` functions work.

**Conceptual expansion.** For a function returning `Ret`, the expression
`value?` behaves as if the compiler inlined:

```dc
if !value.is_ok() {
    return value.unwrap_err(); // defers run, operand not re-evaluated
}
value.unwrap()
```

**Return-type compatibility.** The `.unwrap_err()` return type (`E`) is
checked against the enclosing function's return type (`Ret`) in two steps:

1. Direct assignment of `E` to `Ret` (exact type match or the standard
   assignment-compatibility rules: int widening, float widening, pointer
   qualifier compatibility, etc.).
2. Implicit enum construction: if `Ret` is a tagged enum (`enum` with at
   least one payload variant) and exactly **one** variant is annotated
   `@implicit_construction` with a single payload assignable from `E`, the
   error value is automatically wrapped into that variant.

If neither step succeeds, the compiler emits a diagnostic and rejects the
expression.

**Control flow and `defer`.** On the failure path, all active `defer`
statements (§12) execute in reverse order **before** the early `return`.
On the success path, `.unwrap()` runs and execution continues normally;
the result is merged via a PHI so that the surrounding expression sees the
unwrapped value. The operand expression is lowered exactly once.

---

## 10. Match Expressions

```dc
match value {
    // literal patterns
    0 => handle_zero(),
    1 | 2 | 3 => handle_small(),

    // range patterns
    4..10 => handle_medium(),

    // binding with guard
    x if x > 100 => handle_large(x),

    // tagged enum destructuring
    Result::Ok(inner) => use(inner),
    Result::Err(e) => panic(e),

    // struct destructuring
    Point { x, y => 0 } => on_x_axis(x),
    Point { x, _ } => general(x),

    // nested patterns
    Optional::Some(Point { x, y }) => plot(x, y),

    // wildcard
    _ => default(),
}
```

**Match on pointers:**

```dc
match ptr {
    null => handle_null(),
    p => use(*p), // p is guaranteed non-null here
}
```

---

## 11. UFCS (Uniform Function Call Syntax)

Any free function whose first parameter is `T`, `const T`, `T*`, or `const T*` can be called with dot syntax on a value of type `T`.
When selecting candidates, priority to the exact match followed by auto referencing/dereferencing.

```dc
f64 length(Point* self) { ... }

Point p = Point { x = 3.0, y = 4.0 };
f64 len = p.length(); // auto-reference
```

```dc
f64 length(const Point* self) { ... }
f64 length(Point self) { ... }

Point p = Point { x = 3.0, y = 4.0 };
f64 len = p.length(); // passing by value is preferred
```

```dc
f64 length(const Point* self) { ... }
f64 length(Point* self) { ... }

const Point p = Point { x = 3.0, y = 4.0 };
f64 len = p.length(); // passing const is preferred
```

```dc
f64 length(Point* self) { ... }

const Point p = Point { x = 3.0, y = 4.0 };
f64 len = p.length(); // error
```

**Resolution order:**

1. Struct field access
2. Functions in the current module
3. Functions in imported modules

---

## 12. Defer

```dc
void process_file([]const u8 path) {
    File* f = open(path);
    defer close(f);

    u8* buf = alloc(1024);
    defer free(buf);

    // ... use f and buf ...
}
```

`defer` takes a single statement. Deferred statements execute in reverse order at scope exit. This includes early `return`, `break`, `continue`.

---

## 13. Control Flow

```dc
// if expression or statement
if cond {
    body();
}

if cond {
    a()
} else if other {
    b()
} else {
    c()
}

// while
while cond {
    body();
}

// do-while
do {
    body();
} while cond;

// for c style
for i32 i = 0; i < n; i++ {
    body(i);
}

// for range based for slices
for item in items {
    process(item);
}

// for range based for ranges
for i in 0..n { // exclusive
    body(i);
}
for i in 0..=n { // inclusive
    body(i);
}
```

A range expression (`a..b`, `a..=b`) is only valid directly inside a
`for ... in` loop or a `match` pattern (§10), it is not a general-purpose
first-class value. Index/range bounds default to `usize` unless context forces
another integer type.

---

## 14. Attributes

```dc
@[packed]
struct Header {
    u32 magic;
    u16 version;
}

@align(16)
u8[64] buffer;

@inline
void hot_path() { ... }

@noinline
void cold_path() { ... }

@section(".text.cold")
void in_section() { ... }

@calling_conv("Cdecl")
void cdecl_func() { ... }

@[deprecated("use new_api instead")]
void old_api() { ... }

@nomangle
void c_interop_func(i32 x);

@implicit_construction
i32 implicit_ctor_target() { ... }
```

`[]` can be freely ommitted for one attribute lists. Attributes can also be
attached to individual enum variants (e.g. `@[deprecated("...")] None,`).

`@nominal` (§6.1.1) and `@intrinsic` are two more recognized attributes;
`@intrinsic` marks a declaration as compiler-implemented and is only valid
inside the `core` module.

---

## 15. Function Pointers

```dc
// function pointer type
using BinOp = i32(*)(i32, i32);

i32 apply(BinOp op, i32 a, i32 b) {
    return op(a, b);
}

i32 add(i32 a, i32 b) { return a + b; }

apply(add, 1, 2);
```

---

## 16. Lambda Expressions

A lambda (anonymous function) literal has a compact pipe syntax: an optional
parameter list between `|` and `->`, followed by an expression or block body.

```dc
i32(*)(i32, i32) add = |a, b -> a + b;   // expression body
i32(*)(i32)      twice = |i32 x -> x * 2; // explicit parameter type
i32(*)(i32)      block = |x -> {          // block body
    i32 y = x + 1;
    y
};
i32(*)(i32)      noparams = | -> 42;      // no parameters
```

### 16.1 Syntax

- `|` introduces the lambda. The compact canonical form has no space between
  the pipe and the first parameter (`|x`, never `| x`). An empty parameter
  list is written `| ->`.
- Parameters follow the same rules as function parameters: a bare identifier
  infers the type from context, or an explicit type may be given
  (`|i32 x -> ...`). Multiple parameters are comma-separated (`|a, b -> ...`).
- `->` separates the parameter list from the body and is written with spaces
  around it (`|x -> x`).
- A binary `|` inside the body is ordinary bitwise-or and is spaced like any
  other binary operator (`|x -> x | 1`).

### 16.2 Bodies

The body may be an expression (its value is the lambda's return value) or a
`{ ... }` block (the block's tail expression is the return value). A block
body may contain statements, locals, `return`, and nested lambdas.

### 16.3 Contextual typing

An untyped parameter's type is deduced from the expected function-pointer
type when the lambda appears in one of these positions:

- initializing a function-pointer variable: `i32(*)(i32) f = |x -> x;`
- passing to a function with a function-pointer parameter:
  `apply(|x -> x * 2, 1)`
- an immediate call: `(|x -> x)(5)`

If no function-pointer context is available the compiler emits a focused
"cannot deduce type for lambda parameter" diagnostic. Passing an untyped
lambda through a generic that never invokes it is an error; a fully typed
lambda may pass through freely.

### 16.4 Generic deduction

When a lambda is passed to a template whose parameter is generic (`F`), the
lambda's type is a distinct lambda type. If the generic invokes the callable,
the parameter types are deduced from the invocation and the lambda is
finalized to a function pointer. If the generic never invokes the callable,
the lambda parameters must be explicitly typed.

```dc
void apply(F, Args...)(F f, Args args) {
    f(args...);              // lambda parameter types deduced from `args`
}

apply(|x -> consume(x), 1); // `x` deduced as i32
```

### 16.5 Function-pointer conversion

A typed lambda converts implicitly to the matching function-pointer type and
may be stored, passed, returned, and called through the pointer. Its
generated function has a distinct synthesized symbol identity and never
collides with a source function even if the source happens to be named like
a generated lambda symbol.

### 16.6 Globals

Lambdas may initialize global function-pointer constants and struct fields
of function-pointer type:

```dc
using F = i32(*)(i32);

const F g_double = |i32 x -> x * 2;

struct Handler {
    F h;
}
const Handler g_handler = {h = |i32 v -> v + 1};
```

### 16.7 Non-capture

Lambdas are non-capturing: a lambda body may reference only its parameters,
globals, and values from its own nested scope. Referencing a local variable
of an enclosing function is rejected:

```dc
void bad() {
    i32 local = 1;
    i32(*)(i32) f = |x -> x + local; // error: lambda body cannot capture
}
```

---

## 17. Atomics (`core::atomic`)

`core::atomic` is a compiler-provided module (no import path needed beyond
`import core::atomic;`) exposing atomic memory operations as `@intrinsic`
functions, they're recognized and lowered directly by the compiler, not
ordinary library code.

```dc
public enum MemoryOrder : u8 {
    Relaxed,
    Acquire,
    Release,
    AcqRel,
    SeqCst,
}

public struct Atomic(T) {
    volatile T value;
}
```

Every operation has two overloads: one taking a raw `volatile T*`, and one
taking an `Atomic(T)*` (a struct wrapper around a single `volatile T`
field, for when you want the atomic-ness to be part of a type rather than
threaded through as a separate pointer):

```dc
import core::atomic;
using core::atomic::MemoryOrder;

void example(volatile i32* p, core::atomic::Atomic(i32)* a) {
    i32 v1 = core::atomic::atomic_load(p, MemoryOrder::Acquire);
    i32 v2 = core::atomic::atomic_load(a, MemoryOrder::Acquire);

    core::atomic::atomic_store(p, 1, MemoryOrder::Release);
    core::atomic::atomic_fetch_add(p, 1, MemoryOrder::Relaxed);
    core::atomic::atomic_exchange(p, 2, MemoryOrder::SeqCst);
    core::atomic::atomic_fence(MemoryOrder::SeqCst);
}
```

Available operations: `atomic_load`, `atomic_store`, `atomic_exchange`,
`atomic_fetch_add`, `atomic_fetch_sub`, `atomic_fetch_and`,
`atomic_fetch_or`, `atomic_fetch_xor`, `atomic_fence`.

---
