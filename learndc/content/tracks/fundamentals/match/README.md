# Matching on an enum

A plain enum is a type with a fixed list of named values:

```dc
public enum Light { Red, Yellow, Green }
```

You name a variant with its type: `Light::Red`. A `match` expression picks a
result depending on which variant it is given:

```dc
return match light {
    Light::Red => "Stop",
    Light::Yellow => "Slow down",
    Light::Green => "Go",
};
```

The arms are separated by commas and the whole `match` ends with `;`
because it is an expression. The compiler requires a `match` to be
exhaustive: every variant must be covered. If one is missing, the program
does not build and the error names the missing case.

The pattern `_` matches everything that is left. It makes a `match`
exhaustive, but it also means the compiler stays silent when someone adds a
new variant later: the new variant quietly falls into the `_` arm.

## Your task

`src/traffic.dc` describes a traffic light. The program prints the action and
the number of seconds for each light:

```text
Red: Stop for 30 seconds
Yellow: Slow down for 5 seconds
Green: Go for 25 seconds
```

1. `action` does not build. Read the error and handle the missing variant.
2. After it builds, check the output. `seconds` uses a `_` arm, so one light
   gets the wrong duration without any compiler warning. Replace the wildcard
   with an arm for each variant it stands for.

## Acceptance criteria

- The project builds with `dpm build`.
- The program prints exactly the three lines above and exits with code 0.
- Every light has the right action and the right number of seconds.

Run `learndc check` when you are ready, or `learndc run` to see the output.
