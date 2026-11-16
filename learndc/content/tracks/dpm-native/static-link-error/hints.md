## Hint 1

The linker says it could not find `-l:libm.so.6` and that no library search path was given. Which of the three link modes only uses `lib<name>.a` archives?

## Hint 2

libm.so.6 is a shared object. `Default` links statically unless a library resolves to a shared object, and `Dynamic` prefers shared objects. Either one lets the link use the .so; the product line is the place to change.

## Hint 3

`{ name = "probe", link_mode = LinkMode::Dynamic }`, or delete the link_mode field.
