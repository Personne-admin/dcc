# Shipped anyway

## Case report

The warehouse calls: "Empty boxes and parcels without an address are being shipped. We have a
validator, so someone must have switched it off."

Expected output: valid orders are shipped, the others are rejected and the reason is named.

```text
order 1 shipped
order 2 rejected: no items
order 3 rejected: no address
```

Today all three orders print `shipped`.

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
