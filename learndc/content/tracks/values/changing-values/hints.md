## Hint 1

Each event updates `coins` using the value the last event left behind. A
short form such as `+=` reads the old value, calculates, and stores the
result back.

## Hint 2

Match event to operator: a present adds (`+=`), buying subtracts (`-=`),
doubling multiplies (`*=`), and finding exactly one coin is `++`. The sweet
costs the constant, so subtract `SWEET` rather than a literal 5.

## Hint 3

```dc
coins += 30;
coins -= 15;
coins *= 2;
coins++;
coins -= SWEET;
```

Each line goes directly under its comment, before the matching `println`.
