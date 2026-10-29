# Collections

`array<T>` and `T[]` name the same growable array type. Arrays have shared mutable identity:

```flywheel
int[] values = []
values.push(10)
values.push(20)
print values.pop()   # 20; throws IndexError if empty
print values.length
values.clear()
```

`push(value)` appends a value of the element type; `pop()` removes and returns the last value; `clear()` releases all elements. `len(array)`, indexing, indexed assignment, membership (`value in array`), and `for` iteration are supported. Indexes are checked. Assigning an array to another variable aliases it; mutation is visible through both bindings. A `for` expression is evaluated once and captures the initial length; shrinking the array during iteration can raise `IndexError`. Growth does not extend an already-running loop.

Higher-order methods take function values or lambdas:

```flywheel
var nums = [1, 2, 3, 4]
print nums.map(function(int x) => x * x)              # [1, 4, 9, 16]
print nums.filter(function(int x) => x % 2 == 0)      # [2, 4]
print nums.reduce(0, function(int sum, int x) => sum + x)  # 10
print nums.any(function(int x) => x > 3)              # true
nums.each(function(int x) => print(x))
```

`map` returns a new array of the callback's result type; `filter`, `any`, and `all` require a `bool` predicate. See [function types and closures](language.md).

Maps are homogeneous associative values with shared mutable identity:

```flywheel
map<string, int> scores = {"Danny": 15}
scores["Danny"] += 1
scores["Alice"] = 20
print "Alice" in scores
print scores.remove("Danny")  # bool: whether a key existed
for name in scores do
    print "{name}: {scores[name]}"
end
scores.clear()
```

Keys may be strings, booleans, or integer types. Values may be any supported concrete non-void type, including arrays, tuples, classes, or Results. Missing-key reads raise `KeyError`; plain assignment inserts or updates. A failed RHS does not insert a placeholder. `remove(key)` returns bool, `keys()` returns an array snapshot, and `len(map)` / `.length` count entries. Iteration uses a key snapshot in insertion order. Lookup, insertion, and membership use a hash index over the insertion-ordered entries, so they take expected constant time at any size. Broader key protocols (custom hash/equality) remain future work. Empty maps require a declared/contextual `map<K,V>` type.

Tuples have fixed length and statically known types at each position:

```flywheel
[int, string] pair = [42, "answer"]
print pair[0]
pair[1] = "updated"
var inferred = [true, "heterogeneous"]
[int, string][] rows = []
rows.push([1, "one"])
```

With a tuple context, a bracket literal constructs that tuple. Otherwise, items with a lossless common type infer an array; heterogeneous items infer a tuple. Fixed heterogeneous tuple indices must be compile-time integer expressions, and assignments must preserve the selected field's type. Tuples have shared identity like arrays and support `len` / `.length`; `push`, `pop`, and heterogeneous `for` iteration are not supported.

`collections.range(end)` and `collections.range(start,end)` create an `int[]` for the half-open interval, with step 1. Descending intervals are empty. Generic iterators, sets, custom hash/equality protocols, and the rest of the collections algorithms remain future work.

Homogeneous `tuple<T>` values additionally support dynamic checked indices, membership and iteration, with runtime-determined fixed arity. The `..` and `..<` operators produce `tuple<int>`. See [formatting, casts, repetition, and ranges](value-conversions.md) for constructors and operators.
