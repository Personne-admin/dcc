A test program is an ordinary program whose exit code carries the verdict, so
dpm needs no test framework to run it: build, run, compare the exit code with
0. The printed lines are the report; a good test prints the value it got and
the value it wanted, which is how the first line of the failure already told
you where to look.

Writing the call before the function (the test refers to `lightest_tons`) is
the test-first habit: the compile error is the first failing test. The checks
of this lesson (the program output and a separate harness) do not run your
`dpm test` for you; they test the same behaviour from the outside.
