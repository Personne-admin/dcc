# The function that cannot be found

## Case report

A colleague asks for help: "The build says `unknown path prices::tax_for`. But `tax_for` is
in `prices.dc`, I can see it on the screen and the spelling is right. I imported the module
too. Is the compiler broken?"

The project has two files, `src/main.dc` and `src/prices.dc`. Expected output:

```text
net 100
tax 20
total 120
```

## Your task

1. Reproduce the symptom: build and run the program and compare its output with the expected output.
2. Locate the cause. Answer the question of the first stage before you edit anything: name the line and say why it is wrong.
3. Fix the program in `src/` and check that the output matches exactly.

Do not change the expected output or the data in `main`; fix the code that produces the wrong result.
