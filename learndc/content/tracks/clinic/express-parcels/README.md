# Express at the local rate

## Case report

Customer service says: "We started selling next-day delivery last week. Express parcels are
supposed to cost 15 plus 3 per kilo. The quote for 3 kg is the same as for a parcel to the
next street."

Expected output:

```text
same town: 7 for 3 kg
next county: 14 for 3 kg
abroad: 35 for 3 kg
next day: 24 for 3 kg
```

Today the last line says `next day: 7 for 3 kg`.

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
