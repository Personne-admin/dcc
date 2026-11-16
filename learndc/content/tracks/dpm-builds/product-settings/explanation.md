Settings stack in layers, and the product layer is the second from the top. The
debug profile contributes `-O0`, the product contributes `-O2`, and the higher
layer wins, so `crunch` is optimized while `fleet` is not. A setting on the
project would have changed every product; a profile would have required the
right `--profile` every time. Putting it on the product expresses what is true
of that program: it is compute-bound.

Debug information and bounds checking still come from the profile: the
product only overrode one field.
