# Why the solution works

```dc
public i32 to_fahrenheit(i32 celsius) {
    return celsius * 9 / 5 + 32;
}

public bool is_freezing(i32 celsius) {
    return celsius < 0;
}
```

- `import temperature;` finds `temperature.dc` next to `main.dc`. The import
  alone only makes the module known; what main may use is decided by the
  module itself.
- Top-level declarations are private by default. A private function is not
  part of the module's visible names for other modules, so the compiler
  reports `unknown path temperature::to_fahrenheit`: from `main`'s point of
  view the name does not exist. Adding `public` exports it.
- Calls are qualified with the module name, so it is always clear where a
  function lives. `helper` code stays private and free to change, while the
  `public` functions form the module's interface.
- The prelude (`println`, `Status`, `Error`) is supplied to every user module,
  so `temperature.dc` needs no extra imports.
- Integer arithmetic is exact: `37 * 9 / 5 + 32` is `333 / 5 + 32`, which is
  `66 + 32 = 98`, because integer division truncates.
