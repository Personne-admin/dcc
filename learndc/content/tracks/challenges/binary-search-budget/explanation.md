# Optimize: a comparison budget

Each comparison throws away half of the candidates: 1000 values need at most ten halvings (2^10 = 1024), so the budget of 11 leaves one comparison of slack. A linear scan needs up to 1000. Counting comparisons through an instrumented function is a deterministic measurement: it gives the same number on every machine and does not depend on load, unlike a timer. The half-open range `lo..hi` avoids the classic underflow of `hi = mid - 1` on unsigned indices.
