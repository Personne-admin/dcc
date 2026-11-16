# Collatz steps and peaks

Both functions walk the same sequence. `steps` counts the rounds; `peak` keeps the maximum, initialised with the start value so that a start that is already the highest (like 16) is handled. Parameters are copies, so changing `n` inside the function does not affect the caller. The loop ends because the Collatz sequence reaches 1 for every start that is known to work, 27 included.
