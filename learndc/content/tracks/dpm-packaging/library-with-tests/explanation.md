Writing the test first showed the failure the way a user would meet it: not a
crash, a plausible wrong answer. `max` began with `best = 0`, a value that is
not in the data, so for negatives it returned 0. The fix starts from the first
element, with a guard for the empty slice because there is no first element.

The test belongs in the library's own `tests/` directory because it
tests the library's promises and should travel with the package. It imports
`stats::summary` exactly as an application would, which is what makes it a
check of the public API. The audit in the workspace root exercises the same
functions, so the library is verified from two sides.
