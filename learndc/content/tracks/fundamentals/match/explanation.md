# Why the solution works

```dc
public []const char action(Light light) {
    return match light {
        Light::Red => "Stop",
        Light::Yellow => "Slow down",
        Light::Green => "Go",
    };
}

public i32 seconds(Light light) {
    return match light {
        Light::Red => 30,
        Light::Yellow => 5,
        Light::Green => 25,
    };
}
```

- A `match` over an enum must cover every variant. The error
  `match expression not exhaustive: missing case Light::Yellow` is the
  compiler proving that `action` would have no result for a yellow light.
- Each arm produces a value of the same type, so the whole `match` can be
  returned directly.
- The `_` arm in `seconds` compiled, so the exhaustiveness check was
  satisfied, but it answered `25` for Yellow. The compiler cannot know what
  you meant for the variants a wildcard swallows.
- Listing every variant explicitly keeps the compiler working for you: if
  `Light` gains a fourth variant, both functions fail to build until
  you decide what it does. Use `_` only when the "everything else" behavior
  is truly the same and will stay the same.
