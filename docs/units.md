# Units

Flywheel checks units of measure at compile time. A unit namespace is a dimension such as time or length. A unit is one way to measure it. Values such as `15s` or `0.32km` carry their unit, and mixing incompatible dimensions is a compile error.

```flywheel
unitns time
unit time.s = 1
unit time.min = 60
unit time.h = 3600

unitns metric_length
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

Units apply to numbers, and to every numeric type: any namespace holds `int`, `float`, `double`, and the sized integers (`i32[time]`, `u8[metric_length]`). Other types cannot carry units, so `string[time]` is a compile error.

The `metric`, `imperial`, and `time` library modules declare common units, so most programs import them instead of declaring their own (see [unit libraries](#unit-libraries)):

```flywheel
import metric
import time

float[length] trip = 12.5km
print trip / 30min in kmph       # 25kmph
```

## Declarations

- **Namespaces:** `unitns name` declares a namespace. Every numeric type can hold its quantities; there is no list of allowed types. (The 2.11 form `unitns name for int, float` is now a compile error.)
- **Units:** `unit namespace.unit = factor` declares a unit as a multiple of the namespace's base (the unit with factor 1).
  - **Factors:** a factor is a positive number or a fraction, such as `unit time.ms = 1/1000`.
  - **Exact ratios:** conversions use the exact ratio of the factors as written: `0.01` is exactly 1/100. Converting between units whose ratio is a whole number therefore stays exact.
- **Visibility:** unit names are not global identifiers. They are spelled after a number or through their namespace (`time.min`).
- **Scope of declarations:** declarations go at the top level of a file.
- **Across modules:** a module's namespaces are visible in the modules that import it, so `import metric` makes `float[length]` a type. When two imported modules declare the same namespace name, qualify it with the module: `float[metric.length]`, `float[imperial.length]`. A namespace declared in the importing module hides imported ones of the same name. Unit literals such as `5km` resolve among the namespaces the module declares or imports: `metric` imports `time`, but `1h` still needs `import time` in the module that writes it.

## Values and types

- **Types:** `int[time]` and `float[time]` are the types of quantities in a namespace. Any numeric type may be written before the brackets:
  - **Integer types** (`int`, `i8` ... `i64`, `u8` ... `u64`) hold an integer quantity. Its amount is stored as a 64-bit `int`, whatever width is written.
  - **`float` and `double`** hold a floating-point quantity (64-bit).
- **Literals:**
  - **Writing them:** a number directly followed by a unit is a quantity: `100m`, `0.32km`, `15s`. The unit may be qualified, as in `100metric_length.m`.
  - **Resolving bare units:** a bare unit is resolved from the expected type, or from the only namespace that has it. A unit name in two namespaces must then be qualified.
  - **Numeric kind:** an integer literal is an `int` quantity, or a float quantity where one is expected (`float[time] t = 15s`).
- **Units are kept:** a value keeps the unit it was written in. `15s` stores 15 and seconds, so `unit(x)` is `"s"`.
- **Conversion with `in`:** `x in cm` or `x in metric_length.cm` returns the value in another unit without changing `x`.
  - **Int sources:** converting an `int` quantity gives a float quantity.
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
- **Mixed int and float:** mixing `int` and `float` quantities gives a float quantity.
- **Inexact int addition:** `int` addition needs a whole-number ratio between the units. For example, `1m + 1ft` raises `ValueError`; use `float[length]` for such units.

## Relations between namespaces

`unitconv` relates quantities of three namespaces:

```flywheel
unitns metric_speed
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
- **Floating point:** relations compute in floating point and give float quantities.
- **Namespaces of other modules:** a relation may name namespaces of imported modules, as `metric` does with `time` (`unitconv 1length.m / 1time.s = 1speed.mps`). At least one of the three must be declared in the module, and a quotient's left side must be one of its own (write the relation as a product otherwise). Products work in either order: `2s * speed` uses the relation that `speed * 2s` does.
- **Equivalent namespaces:** `unitconv 1a.x = n b.y` (written `unitconv 1length.ft = 0.3048metric.length.m`) says that two namespaces measure the same thing. Then `+`, `-`, comparisons, and `/` (a plain ratio) accept one quantity of each: the second value is converted to the unit of the first, and the result is a float quantity. `x in y` converts to a unit of the other namespace. One of the two must be declared in the module, and the other may be qualified with its module (`metric.length`).
- **Undeclared products:** a product or quotient with no relation is a compile error. There are no automatic derived dimensions such as `m * m`; declare `unitconv 1length.m * 1length.m = 1area.m2` for area.

## Unit libraries

| Module | Namespaces and units |
|---|---|
| `time` | `time`: `ns`, `us`, `ms`, `s`, `min`, `h`, `d`, `wk` (months and years vary, so they are not units) |
| `metric` | `length`: `nm`, `um`, `mm`, `cm`, `dm`, `m`, `km` · `area`: `mm2`, `cm2`, `m2`, `ha`, `km2` · `volume`: `ml`, `cm3`, `cl`, `dl`, `l`, `dm3`, `m3` · `weight`: `mg`, `g`, `kg`, `t` · `speed`: `mps`, `kmph` |
| `imperial` | `length`: `inch`, `ft`, `yd`, `ch`, `fur`, `mi` · `area`: `sqin`, `sqft`, `sqyd`, `acre`, `sqmi` · `volume`: `floz`, `gi`, `pt`, `qt`, `gal`, `cuin`, `cuft` · `weight`: `oz`, `lb`, `st`, `cwt`, `ton` · `speed`: `fps`, `mph` |

- **`import time.time`:** names the `time` namespace inside the `time` module. It is the same import as `import time`, so writing both is allowed.
- **Sleeping:** `time.sleep` takes a time quantity as well as a number of seconds: `time.sleep 500ms` and `time.sleep delay` (an `int[time]`) convert to seconds.
- **Relations:** `metric` and `imperial` relate length × length = area, area × length = volume, and length / time = speed (they import `time`), so `3m * 4m` is `12m2`, `100m / 10s` is `10mps`, and `60mi / 1h in mph` is `60mph`.
- **Imperial measures:** `imperial` uses British imperial measures: a pint is 20 fluid ounces, a gallon 160, and a ton is the long ton (2240 lb). `in` is a keyword, so inches are `inch`.
- **Between the systems:** `imperial` imports `metric` and relates each of its namespaces to the metric one (`unitconv 1length.ft = 0.3048metric.length.m`), so with both imported `1m + 1ft` is `1.3048m`, `1ft + 1m` is `4.28083989501312ft`, `1m > 1ft` is `true`, and `5ft in cm` is `152.4cm`.

```flywheel
import metric
import imperial
import time

float[imperial.length] run = 5mi
print run in ft, run / 40min in mph      # 26400ft 7.5mph
print 3m * 4m * 2m in l                  # 24000l
```

## Implementation

The module loader lowers each module's units once its imports are loaded (see `src/modules.cpp`). Each namespace becomes hidden classes, `int[time]` and `float[time]`, holding the number and the unit's index; tables of unit names and exact conversion ratios are generated as constants (see `src/units.cpp`). Quantities are ordinary heap objects, so arithmetic on them allocates like any other object.
