# Mastery: Diff Tool

Needs: the whole core curriculum. This challenge combines generics, modules,
owning arrays, custom errors and a classic algorithm. You design the internals;
the public interface is exactly what is listed here.

A *diff* shows what changed between two versions of a sequence. The standard
way to compute it is through the *longest common subsequence* (LCS): the
longest list of elements that appear in both sequences in the same order.
Everything not in it was removed from the first sequence or added to the second.

```text
module greeting;                  module greeting;

public void hello() {             import std::fmt;
    println("hello");
    println("world");             public void hello() {
}                                     println("hello");
                                  }
```

Your tool prints the result like this:

```text
  module greeting;
  
+ import std::fmt;
+ 
  public void hello() {
      println("hello");
-     println("world");
  }
5 unchanged, 1 removed, 2 added
```

## The project

- `src/lcs.dc`, module `lcs`: the generic algorithm (types given, bodies yours).
- `src/lines.dc`, module `lines`: turns text lines into integer ids.
- `src/main.dc`: reads two built-in versions of a file and prints the diff, and
  also prints the LCS length of two words.

The diff is generic: it works on any element type `T` that can be compared with
`==`. A line of text is a slice and cannot be compared with `==`, so `lines`
first replaces every line by a `u32` id (equal text, equal id) and the diff runs
on the ids.

```dc
public const usize MAX_CELLS = 1000000;
public enum EditKind { Keep, Remove, Add }
public struct Edit { EditKind kind; usize index; }
public enum DiffError { @implicit_construction Alloc(AllocError), TooLarge }
```

## `lcs`

`lcs_length(a, b)` returns `Result(usize, DiffError)`: the length of the LCS of the
two slices (`[]const T`, same `T`).

`diff(a, b, out)` returns `Status(DiffError)` and fills `out` with the *edit script*
that turns `a` into `b`:

- `Keep` with `index` into `a`: an element that stays,
- `Remove` with `index` into `a`: an element of `a` that is gone,
- `Add` with `index` into `b`: an element of `b` that is new.

The script lists the edits in order. Walking it, `Keep` and `Remove` consume
`a` one element at a time, and `Keep` and `Add` consume `b`, so every element of
both sequences is used exactly once. The number of `Keep` edits is the LCS length.
`out` is cleared first, but left untouched when the result is an error.

The script is unique because the algorithm is fixed. With `i` and `j` the next
unconsumed elements of `a` and `b`, and `L(i, j)` the LCS length of
`a[i..]` and `b[j..]`:

1. while both sequences have elements left:
   - if `a[i] == b[j]`: `Keep`, advance both,
   - else if `L(i + 1, j) >= L(i, j + 1)`: `Remove` `a[i]`, advance `i`,
   - else `Add` `b[j]`, advance `j`.
2. then `Remove` whatever is left of `a`, then `Add` whatever is left of `b`.

This makes a replacement appear as `Remove` then `Add`, and prefers removals on
ties: `ab` to `ba` is `Remove 0, Keep 1, Add 1`.

`L` is a table of `(a.len + 1) * (b.len + 1)` numbers. To keep memory bounded, when
that product is larger than `MAX_CELLS` both functions return `TooLarge`
*before allocating anything*. Compute the check so it cannot overflow.

## `lines`

`intern(text, table, ids)` appends one id to `ids` for each line of `text`. The
`table` holds the distinct lines seen so far; an id is the index of the line in the
table. A line that is already in the table gets its index; a new line is added to the
table and gets the next index. The table and ids belong to the caller and may already
contain data from an earlier call, so two files can share one table. Equal means
same characters (`std::slice::eq`).

## `main`

Implement `show(old, updated)`: intern both files with one table, diff the id
arrays and print the script as above (`"  "`, `"- "` or `"+ "` before the line
text), then `N unchanged, N removed, N added`. When the diff is `TooLarge`, print
`cannot diff these files`.

## Acceptance criteria

- The project builds with `dpm build`.
- Every check passes, including the exact report from `main`.
