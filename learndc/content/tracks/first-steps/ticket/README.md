# Print a train ticket

You can print text, numbers and several values in one line. Now use all of it
to print a small ticket. Work in three stages and check after each one; the
program does not have to be finished to check a stage.

## The ticket

The finished program prints exactly this:

```text
==== TRAIN TICKET ====
Passenger: Ada Lovelace
From Oslo to Bergen
Seat 14, car 3
Price: 59 euro
Have a good trip, Ada!
======================
```

## Stage 1: the journey

Print the first three lines. Use a placeholder for the route:
`From {} to {}` with `"Oslo"` and `"Bergen"` as arguments.

## Stage 2: seat and price

Add the seat line and the price line. The numbers 14, 3 and 59 are placeholder
arguments, written without quotes.

## Stage 3: the farewell

`Have a good trip, Ada!` is a single line, but write it as three calls: two
with `print` and one with `println`, so that the name goes through a
placeholder in the middle. Then print the closing line of `=` characters.

Edit `src/main.dc`. The three comments in `main` show where each stage goes.

## Acceptance criteria

- The project builds.
- The whole output matches the ticket above exactly and the exit code is 0.
