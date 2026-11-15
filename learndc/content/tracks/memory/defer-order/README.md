# Tidy up in reverse

A party is set up step by step, and everything that was switched on must be
switched off again. `src/main.dc` switches things on but never off:

```dc
println("lights on")?;
println("music on")?;
{
    println("cake served")?;
}
println("party running")?;
```

## Your task

Add three deferred statements, each on the line right after the thing it
undoes, so that the program prints exactly:

```text
lights on
music on
cake served
plates cleared
party running
music off
lights off
```

- `defer println("lights off");` after the lights go on,
- `defer println("music off");` after the music,
- `defer println("plates cleared");` inside the block, after the cake.

Do not move or reorder the existing lines: `defer` produces the order for
you. Remember that a deferred statement has no `?`.
