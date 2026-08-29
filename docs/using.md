# `using`

`using` is a multi-purpose declaration keyword. It's meaning is determined by the
context around it.

## Type aliases

```dc
using Name = Type;
```

Creates a type alias.

```dc
using Vec3 = f32[3];
using Callback = void(*)(i32, i32);
```

## Value aliases

```dc
using Type Name = expression;
```

Binds a name to a compile-time constant value of the given type. The type comes
first, then the binding name, then an `=` initializer. The initializer must be a
compile-time constant and is checked and converted against the declared type.

```dc
using u64 A = 1;
using u64[4] B = { 1, 2, 3, 4 };
using Foo* P = null;
using ns::Foo X = constant_expression;
```

Value aliases may reference other value aliases and compile-time constants, and
may be used from normal runtime expressions, where they evaluate to their
constant value. Aggregate value aliases (arrays and structs) embed the values of
their elements.

A value alias is not an object: it has no storage and no address. `&A` is an
error, and assignment or increment/decrement of a value alias is an error.
Declaring a value alias never emits a global, a `.rodata`/`.data` entry, or an
object symbol. To create an actual object holding the value, use a `const`
object instead:

```dc
using u64[4] Values = { 1, 2, 3, 4 };
const u64[4] stored = Values;
```

Visibility forms work as for type aliases:

```dc
public using u64 A = 1;    // importers see module_name::A
using public u64 B = 2;    // importers see B unqualified
```

## Concepts

```dc
using Name(Params) = compiles(...) { ... };
using Name(Params) = expression;
```

Defines a compile-time predicate usable in template constraints.

```dc
using Addable(T) = compiles(T a, T b) {
    a + b;
};

using Arithmetic(T) = Addable(T) & !PointerLike(T);
```

## Bare imports

```dc
using path::name;
```

Brings one existing binding into the current scope under its original name.

```dc
using math::sin;
```

## Wildcard imports

```dc
using path::*;
```

Brings all exported bindings from a module or namespace into the current scope.

```dc
using math::*;
```

## Aliased wildcard imports

```dc
using Alias = path::*;
```

Creates a namespace group alias for all bindings in the target namespace.

```dc
using math_group = math::*;
// math_group::sin, math_group::cos, ...
```

## List imports

```dc
using { path::a, path::b };
using prefix::{a, b};
```

Brings several selected bindings into the current scope.

```dc
using { math::sin, math::cos };
using lib::{stdout, write};
```

Lists may be nested:

```dc
using lib::{io::{stdout, stderr}, sys::{write, read}};
```

## Aliased list imports

```dc
using Alias = { path::a, path::b };
```

Creates a namespace group containing the selected bindings.

```dc
using math_group = {math::sin, math::cos};
// math_group::sin, math_group::cos
```

## Path and module aliases

```dc
using Alias = path::target;
```

Creates an alias to an existing binding. The target may be a type, value,
function overload set, namespace, or module.

```dc
using V = math::Vec3;
using platform = std::win;
```

## Namespace

```dc
using Namespace::name = target;
```

Binds an existing target into a namespace path. This is used to attach free
functions to a type namespace for namespaced construction for syntax sake.

```dc
public Pair make(i32 a, i32 b) {
    return Pair { first = a, second = b };
}

public using Pair::make = make;
// Pair::make(1, 2)
```

## Visibility forms

```dc
public using Alias = target;
```

Re-exports the alias normally. Importers can access it through this module's
namespace, for example `this_module::Alias`.

```dc
using public Alias = target;
```

Spills the alias into importers' scopes. Importers can access `Alias`
unqualified after importing this module.

```dc
using public name;
```

Spills an already-resolved binding under its original name.
