# Units

Flywheel checks units of measure at compile time. A unit namespace is a dimension such as time or length. A unit is one way to measure it. Values such as `15s` or `0.32km` carry their unit, and mixing incompatible dimensions is a compile error.

```flywheel
unitns time for int, float
unit time.s = 1
unit time.min = 60
unit time.h = 3600

unitns metric_length for int, float
unit metric_length.cm = 0.01
unit metric_length.m = 1
unit metric_length.km = 1000

int[metric_length] length = 100m
float[metric_length] distance = 0.32km
print length                     # 100m
print distance in cm             # 32000cm

int[time] duration = 15s
print type(duration), unit(duration), unitns(duration)   # int s time
print duration in time.min       # 0.25min
```

## Declarations

- **Namespaces:** `unitns name for int, float` declares a namespace and the numbers it may hold. `float` and `double` both mean 64-bit floating point here.
- **Units:** `unit namespace.unit = factor` declares a unit as a multiple of the namespace's base (the unit with factor 1).
  - **Factors:** a factor is a positive number or a fraction, such as `unit time.ms = 1/1000`.
  - **Exact ratios:** conversions use the exact ratio of the factors as written: `0.01` is exactly 1/100. Converting between units whose ratio is a whole number therefore stays exact.
- **Visibility:** unit names are not global identifiers. They are spelled after a number or through their namespace (`time.min`).
- **Scope of declarations:** declarations go at the top level of a file. Units are local to the module that declares them.

## Values and types

- **Types:** `int[time]` and `float[time]` are the types of quantities in a namespace.
- **Literals:**
  - **Writing them:** a number directly followed by a unit is a quantity: `100m`, `0.32km`, `15s`. The unit may be qualified, as in `100metric_length.m`.
  - **Resolving bare units:** a bare unit is resolved from the expected type, or from the only namespace that has it. A unit name in two namespaces must then be qualified.
  - **Numeric kind:** an integer literal is an `int` quantity, or a float quantity where one is expected (`float[time] t = 15s`).
- **Units are kept:** a value keeps the unit it was written in. `15s` stores 15 and seconds, so `unit(x)` is `"s"`.
- **Conversion with `in`:** `x in cm` or `x in metric_length.cm` returns the value in another unit without changing `x`.
  - **Int sources:** for `int` quantities the result is a float quantity when the namespace allows float.
  - **Int-only namespaces:** in a namespace that only allows `int`, the conversion must be whole, or it raises `ValueError`.
- **Introspection:** `type(x)` gives the number type (`int` or `float`), `unit(x)` the unit, and `unitns(x)` the namespace. `x.value` is the plain number in the value's own unit. `type(x)` also works on any other value (`type("a")` is `string`).
- **Printing:** quantities print with their unit, also inside collections: `[1m, 50cm]`.
- **Functions:** a quantity passed to a function keeps its unit. A parameter `int[time]` accepts any time unit.
- **Collections:** quantities work in collections and as set elements and map keys. `100cm` and `1m` are equal.

## Arithmetic

| Expression | Result |
|---|---|
| `a + b`, `a - b` (same namespace) | in the finer of the two units: `100m + 2km` is `2100m` |
| `a < b`, `a == b`, ... (same namespace) | compares the amounts: `1m == 100cm` is `true` |
| `a * n`, `n * a`, `a / n`, `a // n` | scales the amount; `/` gives a float quantity, `//` keeps `int` |
| `a / b` (same namespace) | a plain `float` ratio: `100m / 2km` is `0.05` |
| `-a` | negates the amount |
| `a * b`, `a / b` (different namespaces) | uses a `unitconv` relation, see below |

- **Rejected at compile time:**
  - Adding or comparing quantities of different namespaces: `100m + 10s`.
  - Mixing a quantity with a plain number in `+`, `-`, or comparisons: `5s + 1` needs a unit, such as `5s + 1s`.
  - Assigning a plain number to a quantity variable, or a quantity to a number variable. Use `.value` for the number.
- **Mixed int and float:** in a namespace that allows both, mixing `int` and `float` quantities gives a float quantity.
- **Inexact int addition:** `int` addition needs a whole-number ratio between the units. For example, `1m + 1ft` raises `ValueError`; use `float[length]` for such units.

## Relations between namespaces

`unitconv` relates quantities of three namespaces:

```flywheel
unitns metric_speed for int, float
unit metric_speed.mps = 1
unit metric_speed.kmph = 0.2777777777777778

unitconv 1metric_length.m / 1time.s = 1metric_speed.mps

var speed = 100m / 10s
print speed, speed in kmph       # 10mps 36kmph
print speed * 2s                 # 20m
print 72km / 10metric_speed.mps  # 7200s
```

- **Inverse relations:** a relation also gives its inverse forms. From `m / s = mps` follow `mps * s = m`, `s * mps = m`, and `m / mps = s`.
- **Result unit:** results are expressed in the unit that the relation names for them.
- **Floating point:** relations compute in floating point, so their namespaces must allow `float`.
- **Undeclared products:** a product or quotient with no relation is a compile error. There are no automatic derived dimensions such as `m * m`; declare `unitconv 1length.m * 1length.m = 1area.m2` for area.

## Implementation

Each namespace becomes hidden classes, `int[time]` and `float[time]`, holding the number and the unit's index; tables of unit names and exact conversion ratios are generated as constants (see `src/units.cpp`). Quantities are ordinary heap objects, so arithmetic on them allocates like any other object.
