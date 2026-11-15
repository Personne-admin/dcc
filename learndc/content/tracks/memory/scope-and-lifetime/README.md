# Scopes, lifetimes and defer

So far your variables simply existed while a function ran. This lecture
looks at *when* a value stops existing, and at the things that must be
cleaned up before that happens.

## Every value has a lifetime

A pair of braces `{ ... }` is a *block*, and a block is a *scope*: a variable
declared inside it can only be used inside it, and it disappears when the
block ends. Function bodies, loop bodies and the branches of an `if` are all
blocks, and you can open one anywhere with plain braces.

```dc example=block_scope
```

`total` lives in the whole of `main`. `bonus` lives only in the inner block;
using it after the closing brace is a compile error (`unknown name`). Picture
scopes as nested boxes:

```text
main  +--------------------------------------+
      | total                                 |
      |   block +------------------------+    |
      |         | bonus                  |    |
      |         +------------------------+    |
      |   (bonus is gone here)                |
      +--------------------------------------+
```

The end of a scope is not only a matter of names. It is the moment the
values in it are gone for good.

## Some things need cleaning up

Many things a program uses come in pairs: a door you open must be closed, a
file you open must be closed, memory you borrow from the system must be
given back. Forgetting the second half is a *leak*: nothing crashes, but the
resource is lost until the program ends.

The classic way to forget it is an early exit:

```dc
Status(Error) visit(i32 guests) {
    println("door opened")?;
    if guests == 0 {
        return Status::Ok;          // the door stays open!
    }
    println("{} guests inside", guests)?;
    println("door closed")?;
    return Status::Ok;
}
```

Every new `return`, and every `?` that can pass an error upward, is one more
exit where the cleanup has to be repeated. Sooner or later one is missed.

## defer: write the cleanup once

`defer` takes one statement and runs it when the enclosing scope ends,
however it ends: at the closing brace, at a `return`, or when `?` passes an
error upward. Write it right after the thing it cleans up:

```dc example=defer_once
```

The deferred `println` has no `?`. By the time it runs, the function is
already on its way out and cannot report an error any more, so never write
`?` inside a deferred statement.

## Several defers run in reverse

When a scope has several deferred statements, they run last-in, first-out,
like taking plates off a stack. That is exactly the order teardown needs:
the fire was lit after the tent was pitched, so it is put out first.

```dc example=defer_order
```

The loop body is a block of its own, so its `defer` runs at the end of each
day, not at the end of `main`.

## Common mistakes

- Writing the `defer` far away from what it cleans up. Put it on the very
  next line, so a reader sees the pair at once.
- Writing `?` inside a deferred statement.
- Expecting a `defer` in a loop body to wait for the end of the function.
  It runs at the end of every iteration.

Next you will practise exactly these rules, and then use `defer` for the
most important cleanup of all: memory you own.
