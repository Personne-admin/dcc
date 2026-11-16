# Seventy megabytes that became six

## Case report

A user writes: "Your size tool is fine for small files. A 70 MB file shows up as 6144 KB, that is 6 MB.
The total at the bottom is far too small as well."

Expected output:

```text
3000 bytes = 2 KB
5242880 bytes = 5120 KB
73400320 bytes = 71680 KB
total 76802 KB
```

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
