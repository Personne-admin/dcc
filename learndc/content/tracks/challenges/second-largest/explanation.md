# The second largest value

Three cases per element: it beats the maximum, it falls strictly between the second and the maximum, or it changes nothing. Strictness is what makes duplicates of the maximum harmless. The boolean flag replaces a magic initial value: any number could be a legitimate answer (0, -1, the smallest `i32`), so the "no second value yet" state needs its own variable, and `Optional` is how the function reports "no answer" without inventing one.
