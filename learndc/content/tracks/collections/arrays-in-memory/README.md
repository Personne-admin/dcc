# Arrays and slices in memory

Until now every variable held one value. A program usually has to keep many
values of the same kind: the temperatures of a week, the letters of a name,
the scores of a game. Making one variable per value does not scale. An
*array* is one variable that holds a fixed number of values in a row.

## An array is a row of boxes

`i32[4]` is an array of exactly four `i32` values. The size is part of the
type, so the compiler always knows how much room it needs. The values sit
next to each other in one block of memory:

```text
temps  (type i32[4], 16 bytes)

   +------+------+------+------+
   |  12  |  15  |   9  |  14  |
   +------+------+------+------+
     [0]    [1]    [2]    [3]
```

Each box has an *index*, and the first one is index 0. The last valid index
of an array with N elements is N - 1. Writing `temps[2]` reads or changes
the third box.

```dc example=array_basics
```

An array does not carry a `.len`. Its length lives in its type, so give the
length a name once with a `const` and use that name for the declaration and
for every loop. If the braces hold more or fewer values than the size
says, the compiler reports an error. `i32[3] zeros = {};` fills the whole
array with zeros.

## A slice is a window

A function should not care whether its numbers came from an array of four
or an array of four hundred. For that there is the *slice*: `[]const i32`
is a pointer to the first element plus a length, a window onto values that
live somewhere else. It copies nothing and owns nothing.

```text
temps   +----+----+----+----+----+
        | 12 | 15 |  9 | 14 | 20 |
        +----+----+----+----+----+
                 ^
middle  ptr -----+   len 3   (temps[1..4])
```

```dc example=slice_window
```

- An array converts to a slice by assignment or when it is passed to a
  function that takes a slice. The slice then has a `.len`.
- `temps[1..4]` is a slice of indexes 1, 2 and 3. The end is not included,
  exactly like the ranges in `for` loops.
- `[]const i32 primes = { 2, 3, 5, 7 };` writes a slice straight from a
  list of values. `const` means the elements cannot be changed through it.

## Out of range stops the program

What happens when you read `temps[4]`? There is no fifth box: that memory
belongs to something else. DC programs built by `dpm` check every index.
A wrong index stops the program at once with a message such as
`panic: array index out of bounds` and the line of the access. That is a
bug in your program, found early, and a much better outcome than quietly
reading junk.

## Common mistakes

- Counting from 1. The first element is `values[0]`, and the last of
  `i32[4]` is `values[3]`.
- Looping with `0..=DAYS`. The end of a range is excluded, so `0..DAYS`
  visits every valid index and `0..=DAYS` visits one too many.
- Expecting `temps.len` on a fixed array. Name the size with a `const`, or
  assign the array to a slice first.
- Treating a slice as a copy. It is a window: a slice of the whole array and
  the array see the same values.
