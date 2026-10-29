# Collections

## Arrays

`array<T>` and `T[]` name the same growable array type. Arrays have shared mutable identity: assigning an array to another variable aliases it, and mutation is visible through both bindings. Indexes are checked.

```flywheel
int[] values = [5, 3, 9]
values.push(1)
values.sort()                         # [1, 3, 5, 9]
values.sort(function(int a, int b) => a > b)
print values.slice(1, 3), values[0..1], values.index_of(9)
values.insert(0, 42)
print values.remove_at(0), values.remove(3), values.join(", ")
```

| Method | Behavior |
|---|---|
| `push(x)`, `pop()` | append; remove and return the last item (`IndexError` when empty) |
| `insert(i, x)` | insert before position `i` (0 to length) |
| `remove_at(i)` | remove and return the item at `i` |
| `remove(x)` | remove the first equal item; returns whether one was found |
| `clear()` | remove everything |
| `extend(other)` | append every item of another sequence of the same item type |
| `sort()` | stable in-place sort of numbers, chars, bools, or strings |
| `sort(less)` | stable sort with `function(T a, T b) returns bool` meaning "a goes before b" |
| `reverse()` | reverse in place |
| `slice(start)`, `slice(start, end)` | new array; negative indices count from the end; bounds are clamped |
| `index_of(x)`, `contains(x)` | first index or -1; membership (also `x in array`) |
| `copy()` | shallow copy |
| `join(separator = "")` | concatenate items; non-strings use their `string(...)` form |
| `map`, `filter`, `reduce(initial, f)`, `any`, `all`, `each` | higher-order helpers |

`a[i..j]` and `a[i..<j]` slice like `slice` with an inclusive or exclusive end. Tuples (`tuple<T>` and `[A, B]`) support the read-only methods (`slice`, `index_of`, `contains`, `copy`, `join`); mutating methods require an array.

A `for` expression is evaluated once and captures the initial length; shrinking the array during iteration can raise `IndexError`. `for index, item in values` also binds the index.

## Maps

Maps are homogeneous associative values with shared mutable identity, insertion-ordered and hash-indexed:

```flywheel
map<string, int> scores = {"Danny": 15}
scores["Danny"] += 1
scores["Alice"] = 20
print "Alice" in scores, scores.remove("Danny"), scores.keys(), scores.values()
for name, score in scores do
    print "{name}: {score}"
end
```

Keys may be integers, `bool`, `char`, enums, `string`, tuples of these (structural equality and hashing), or classes that define `hash() returns int` and `equals(T other) returns bool`. Missing-key reads raise `KeyError`; plain assignment inserts or updates. `remove(key)` returns whether a key existed, `keys()` and `values()` return array snapshots, and `len(map)` / `.length` count entries. Iteration uses a snapshot in insertion order. Lookup, insertion, and membership take expected constant time. Empty maps need a declared or contextual `map<K,V>` type.

```flywheel
class Point
    int x
    int y
    function hash() returns int
        return inst x * 31 + inst y
    end
    function equals(Point other) returns bool
        return inst x == other.x and inst y == other.y
    end
end
map<Point, string> labels = {Point(1, 2): "a"}
map<[int, string], int> pairs = {[1, "a"]: 10}
print Point(1, 2) in labels, labels[Point(1, 2)]
```

A class with `equals` also uses it for `==` and `!=` between related class values. Class keys call `hash` and `equals` during lookup; an error raised by either propagates normally.

## Sets

`set<T>` holds unique elements in insertion order, with the same element rules as map keys:

```flywheel
set<int> seen = {3, 1, 3}         # {3, 1}
seen.add(5)
print 1 in seen, seen.remove(3), seen.length
var letters = set("hello")        # set<char>
print seen.union({9}), seen.intersection({1}), seen.difference({1}), {1}.is_subset(seen)
for x in seen do
    print x
end
```

`set(sequence)` builds a set from an array, tuple, string, map (its keys), or another set; `set<T>()` or `{}` with an expected type makes an empty one. Methods: `add`, `remove` (returns bool), `contains` (also `in`), `clear`, `items()` (array snapshot), `union`, `intersection`, `difference`, and `is_subset`.

## Iterator protocol

A class becomes iterable by providing `next() returns T?`, or `iterator()` returning such an object. `for x in object` calls `next()` until it returns null:

```flywheel
class Countdown
    int n
    function next() returns int?
        if inst n == 0 then
            return null
        end
        inst n -= 1
        return inst n + 1
    end
end
for n in Countdown(3) do
    print n    # 3, 2, 1
end
```

## Tuples and ranges

Tuples have fixed length and statically known types at each position:

```flywheel
[int, string] pair = [42, "answer"]
print pair[0]
pair[1] = "updated"
var inferred = [true, "heterogeneous"]
```

With a tuple context, a bracket literal constructs that tuple. Otherwise, items with a lossless common type infer an array; heterogeneous items infer a tuple. Fixed heterogeneous tuple indices must be compile-time integer expressions. Homogeneous `tuple<T>` values additionally support dynamic checked indices, membership and iteration, with runtime-determined fixed arity. The `..` and `..<` operators produce `tuple<int>`.

`collections.range(end)` and `collections.range(start,end)` create an `int[]` for the half-open interval, with step 1. See [formatting, casts, repetition, and ranges](value-conversions.md) for constructors and operators.
