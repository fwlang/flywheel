# Formatting, casts, repetition, and ranges

`string(value)`, `print value`, and string interpolation format all supported scalar and managed value types. Arrays, maps, tuples, class fields, and Result payloads can be nested:

```flywheel
map<string, int[]> scores = {"group": [1, 2]}
print string(scores)             # {"group": [1, 2]}
print [true, "hello", scores]    # (true, "hello", {"group": [1, 2]})
```

Arrays use brackets, tuples use parentheses (including a trailing comma for a singleton), maps preserve insertion order, and strings inside collections are quoted and escaped. Top-level string values stay unquoted. Classes format as `Name(field=value, ...)`; Results format as `Result.ok(...)` or `Result.error(...)`. HTML elements/fragments still serialize as HTML. Native JSON values use JSON serialization, errors format as their message, and function references use a symbolic function name rather than an address. Cycles in ordinary object/collection graphs print `<cycle>`; this does not collect strong-reference cycles.

This representation is for display, not a promise of JSON or round-trip source syntax. Use the JSON library for JSON documents. Collection/class formatting has a nesting limit of 256.

## Collection constructors and checked casts

`array<T>` is an alias for `T[]`. `tuple<T>` is a homogeneous tuple whose length can be determined at runtime; `[T1,T2,...]` remains a fixed heterogeneous tuple type.

```flywheel
print array("hello")                 # ["hello"], not characters
print tuple("hello")                 # ("hello",)
print array(42)                      # [42]
array<int> numbers = array<int>(["1", "2"])
array<array<int>> rows = array<array<int>>([["1"], ["2", "3"]])
[int, string] pair = tuple(["42", 2]) # (42, "2")
map<string, int> scores = map<string, int>([["a", "1"], ["b", "2"]])
```

- `array(value)` and `tuple(value)` treat scalar strings as one value. They copy the outer container when given a sequence. Same-typed nested objects/collections keep shared identity; a conversion that changes an element's type recursively converts that element.
- A heterogeneous tuple needs an explicit homogeneous target type when its items have no lossless common type, e.g. `array<string>([42, "answer"])`.
- Casting a map to an array or tuple produces two-item key/value tuples, preserving entries rather than discarding values. Casting a sequence of pairs to `map<K,V>` checks pair lengths and converts both keys and values. Duplicate converted keys use the last value, preserving the first insertion position.
- `array()` and `map()` need a declared/contextual type, or explicit type arguments. `tuple()` creates an empty tuple. `array<T>()`, `tuple<T>()`, and `map<K,V>()` create typed empty values.
- Explicit collection casts use an expected declaration/assignment/parameter/return type when provided. They do not change the rules for implicit assignment conversions.
- Numeric casts accept numbers, checked numeric strings, booleans as 0/1, and recursively nested **single-item** sequences. Multi-item sequences raise `CastError`; numeric range/parse failures raise `NumericError`. Strings are never implicitly parsed as maps or JSON.
- `bool(value)` is explicit: numbers test nonzero, collections test nonempty, null tests false, and strings must be `true`, `false`, `1`, or `0`. Other non-null managed objects test true. Conditions still require bool and do not gain implicit truthiness.
- An optional scalar can be wrapped without losing nullability, e.g. `array(tree.query("missing"))` produces an `element?[]` containing null. A cast to a non-null target checks it. Unsupported type combinations and missing inference context produce diagnostics rather than silently dropping data.

Collection conversion nesting is limited to 128. Arbitrary user generics, callable element types, and `char` remain outside the supported type set.

## Repetition

`collection * count` and `count * collection` repeat arrays, tuples, and strings:

```flywheel
print [1, 2] * 3        # [1, 2, 1, 2, 1, 2]
print "ab" * 2          # abab
print [1, "x"] * 2      # (1, "x", 1, "x")
```

Repetition makes a new outer collection and retains shared references to its items. It does not clone each object. Zero produces an empty result; negative counts or overflowing sizes fail. A heterogeneous tuple needs a nonnegative constant count so its position types remain known (up to 4096 positions). A homogeneous tuple accepts a runtime count. Maps reject repetition because a map cannot contain duplicate keys.

## Range operators

`first..last` includes the last endpoint. `first..<last` excludes it. Both eagerly construct `tuple<int>` values with unit step:

```flywheel
print 1..3              # (1, 2, 3)
print 1..<3             # (1, 2)
int first = 2
int last = 4
var values = first..last
for value in values do
    print value
end
print values[1]         # 3; checked dynamic indexing
```

Endpoints are signed 64-bit integers after lossless widening. Descending ranges are empty; equal endpoints yield one item for `..` and none for `..<`. Cardinality overflow raises `NumericError`, including the inclusive signed-minimum to signed-maximum range. The implementation avoids computing an overflowing `last + 1`.

Arithmetic binds more tightly than range operators, which bind more tightly than comparisons/membership: `1..2+1` is `1..3`. Use `print((1..3) * 2)` to repeat a parenthesized range inside an explicit print call. Tuple arity cannot change after construction, though values can be assigned at checked homogeneous indices. Existing `collections.range` still returns an array.
