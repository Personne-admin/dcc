# Values and types

Programs work with information: a name, an age, a price. This lecture gives
that information its proper words.

## Values, variables and types

A *value* is a single piece of information, like `12`, `2.5`, `true` or
`"Ada"`. A *variable* is a named place that holds a value, so the program can
use it again later. Creating one is a *declaration*:

```dc
i32 age = 12;
```

The parts, from left to right: the *type* `i32`, the name `age`, an `=`, the
value `12` and the closing `;`. The type comes first in DC.

A *type* says what kind of value a variable holds. Numbers, letters and text
are different kinds of thing, and the compiler keeps them apart.

## The basic types

```dc example=types_tour
```

| Type | Holds | Example |
|---|---|---|
| `i32` | whole numbers, positive or negative | `-5`, `12` |
| `u8` | small whole numbers from 0 to 255 | `255` |
| `i64` | very large whole numbers | `3000000000` |
| `f64` | numbers with a decimal point | `2.5` |
| `bool` | yes or no | `true`, `false` |
| `char` | one letter or symbol, in single quotes | `'A'` |
| `[]const char` | text, in double quotes | `"Ada"` |

The `i` in `i32` means *integer that may be negative*. The `u` in `u8` means
*unsigned*: zero and up. The number is how many bits the value takes, and a
decimal `f64` prints with six digits after the point, so `2.5` appears as
`2.500000`.

## How big a number may be

More bits mean bigger numbers and more room in memory. The compiler can tell
you how many bytes (a byte is 8 bits) a type takes:

```dc example=type_sizes
```

| Type | Bytes | Range |
|---|---|---|
| `u8` | 1 | 0 to 255 |
| `i32` | 4 | about -2.1 billion to 2.1 billion |
| `i64` | 8 | about -9 quintillion to 9 quintillion |

When in doubt about a whole number, use `i32`. Use `i64` when it can exceed
two billion. A value that does not fit is an error, and the compiler checks
literals for you: `u8 level = 300;` is refused, and so is
`i32 big = 3000000000;`.

## Why the compiler checks types

A type is a promise about what the variable holds. Because the compiler
checks every value against it, a whole class of mistakes cannot reach a
running program: text in a number, a decimal where a whole number is needed,
a yes/no used as a count.

The two sides of a declaration must agree, and so must the two sides of an
operator. Here `count` is an `i32`, `4` fits an `i32` and the sum is an `i32`
again, so every step is consistent:

```dc example=same_type
```

Adding a `u8` variable to an `i32` variable is an error until one of them is
converted; a later lesson shows how. The compiler tells you instead of
guessing.

## Common mistakes

- Using double quotes for a `char` or single quotes for text: `'A'` is a
  `char`, `"A"` is text.
- Putting a decimal into an integer: `i32 x = 3.5;` is an error.
- Putting a number too large into a small type, such as `u8 x = 300;`.
- Writing the name before the type: the type always comes first.

Next, you will declare variables and fix a type mismatch yourself.
