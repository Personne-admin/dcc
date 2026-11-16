# Project: Unit Converter

Needs: the data and errors chapters (structs, enums, `Optional`, `Result`,
custom error types) and the formatting lesson.

A workshop needs a converter that knows a few units and refuses nonsense such
as "5 kilograms to metres". `src/units.dc` holds a table of units and the
shapes you work with; `src/main.dc` holds seven conversion requests.

```dc
public struct Unit {
    []const char name;
    Dimension dimension;   // Length, Mass or Temperature
    f64 factor;
    f64 offset;
}

public enum ConvertError { UnknownUnit, DimensionMismatch }
```

## One formula for every unit

Each unit is described against the base unit of its dimension (metres,
kilograms, kelvin) by two numbers:

```text
base  = value * factor + offset
value = (base - offset) / factor
```

Kilometres have factor 1000 and offset 0. Celsius has factor 1 and offset
273.15 (kelvin). Fahrenheit has factor 5/9 and an offset that makes 32 F
equal 273.15 K. A conversion is two steps: to the base unit, then out of it.
`UNITS` is already filled in.

## Stage 1: Look a unit up

`find_unit(name)` returns `Optional::Some(index)` where `UNITS[index]` has
exactly that name (case sensitive), otherwise `Optional::None`. Compare text
with `std::slice::eq`.

## Stage 2: Through the base unit

`to_base(unit, value)` and `from_base(unit, base)` implement the two formulas.

## Stage 3: Convert with refusals

`convert(value, from, to)` returns `Result(f64, ConvertError)`:

- `UnknownUnit` if either name is not in the table,
- `DimensionMismatch` if the units measure different things,
- otherwise the converted value.

## Stage 4: Print a conversion table

`print_table(from, to, values)` prints `from -> to`, then one row per value.
The first refused conversion prints one line through the provided
`print_error` and ends the table.

```text
C -> F
      0.00 C =     32.000 F
     37.00 C =     98.600 F
kg -> m
1.00 kg to m: different kinds of unit
```

## Stage 5: The conversion desk

`main` handles every request in order. A success prints
`12.50 km = 7.77 mi`; a refusal uses `print_error`. After the loop print how
many conversions were done and refused, then the temperature table for
`C` to `F`:

```text
12.50 km = 7.77 mi
100.00 C = 212.00 F
3.00 lb = 1.36 kg
72.00 in = 6.00 ft
5.00 kg to m: different kinds of unit
1.00 furlong to m: unknown unit
0.00 K = -273.15 C
5 converted, 2 refused
C -> F
      0.00 C =     32.000 F
     10.00 C =     50.000 F
     37.00 C =     98.600 F
    100.00 C =    212.000 F
```
