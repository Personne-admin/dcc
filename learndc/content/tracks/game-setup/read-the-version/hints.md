## Hint 1

Both functions are small. The version is three integer expressions on `packed`; the text conversion is a loop that finds where the C string ends.

## Hint 2

Version: `packed / 1000000` is the major number, `(packed / 1000) % 1000` the minor number and `packed % 1000` the patch number. Text: return `""` for null, count with `while text[n] != (0 as char) { n += 1; }` and then slice the pointer.

## Hint 3

```
return Version { major = packed / 1000000, minor = (packed / 1000) % 1000, patch = packed % 1000 };
```

```
if text == null { return ""; }
usize n = 0;
while text[n] != (0 as char) { n += 1; }
return core::slice::from_raw_const(text, n);
```
