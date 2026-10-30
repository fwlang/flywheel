# Classes, interfaces, enums, generics, and pattern matching

## Classes and constructors

A class declares fields and methods. Inside a method, `inst` names the receiver: `inst name` reads a field, `inst name = value` assigns it, and `inst method(...)` calls another method (with dynamic dispatch, see below).

```flywheel
class Person
    string name
    int age = 0

    function greet() returns string
        return "Hello, {inst name}!"
    end
end

var ann = Person("Ann", 30)   # generated constructor: fields in declaration order
var bo = Person "Bo"          # outer call without parentheses; age uses its default
```

A bare class name is a **class reference**, not an instance: `var Make = Person` stores the class, and `Make("Cy")` or `Make "Cy"` constructs. Construction always needs a call: `Person()`, `Person(args)`, or the outer form `Person args`. The same rule applies to native classes such as `desktop.window`. A class-reference statement on its own line (`Person`) is rejected because it does nothing.

Class references convert to function types. A constructor's natural type takes the `init` parameters, or every field in order; trailing parameters with defaults may be left out of the target type:

```flywheel
function(string) returns Person make = Person   # age has a default
var makers = [Person, Person]                    # array<function(string, int) returns Person>
print makers[0]("Dee", 4)
```

### Custom `init`

Defining `function init(...)` replaces the generated constructor. Fields with defaults are set before `init` runs; `init` must then assign every field without a default.

```flywheel
class Temperature
    double celsius
    string label = "temperature"

    function init(double fahrenheit)
        inst celsius = (fahrenheit - 32.0) * 5.0 / 9.0
    end
end
print Temperature(212.0).celsius   # 100
```

Definite initialization is checked conservatively at compile time. Required fields must be assigned by **top-level** statements of `init` (or by a top-level `super.init(...)`, which covers the base class's fields) before `init` reads `inst`, calls a method, or can return. Violations are compile errors such as `init must assign required field 'x'` or `init uses 'inst' before assigning required field 'x'`. `init` cannot return a value. `Class.from_json` builds instances field by field and does not run `init`.

### Member visibility and readonly fields

Fields and methods are public by default. A `private` member is visible only in the methods of its own class (not in subclasses or other code). A `readonly` field can be set by the generated constructor and by assignments in `init` of its own class; any other assignment is a compile error.

```flywheel
class Account
    readonly string id
    private int balance = 0
    function deposit(int amount)
        inst balance += amount
    end
    function total returns int
        return inst balance
    end
end
var a = Account("acct-1")
a.deposit(5)
print a.id, a.total()    # a.balance and a.id = "x" do not compile
```

`readonly` makes the field's binding immutable; a collection stored in it can still change unless it is [frozen](collections.md#frozen-collections).

### Operators and text

A class can define operators as methods named `operator` followed by the symbol:

| Method | Used by |
|---|---|
| `operator +(T other)`, `-`, `*`, `/`, `//`, `%` | `a + b`, ..., and compound assignment `a += b` |
| `operator -()` | unary `-a` |
| `operator ==(T other) returns bool` | `a == b`, and `a != b` as its negation |
| `operator <(T other) returns bool` | `a < b`; also `>`, `<=`, `>=` unless those are defined |
| `operator [](K key)` / `operator []=(K key, V value)` | `a[key]` and `a[key] = value` |

```flywheel
class Money
    readonly int cents
    function operator +(Money other) returns Money
        return Money(inst cents + other.cents)
    end
    function operator <(Money other) returns bool
        return inst cents < other.cents
    end
    function to_string returns string
        return "${inst cents // 100}.{inst cents % 100:02}"
    end
end
print Money(150) + Money(275)    # $4.25
```

The left operand selects the method, so `money * 2` works with `operator *(int k)` but `2 * money` does not. A class without `operator ==` keeps identity comparison (or `equals`, when it defines one). `function to_string returns string` sets how `print`, `string(x)`, and interpolation show an instance.

## Inheritance

`class Dog extends Animal` inherits fields and methods. The base class's fields come first in the subclass's field order (and its generated constructor). A subclass may override a method with exactly the same parameter and return types; a different signature is a compile error. `super.method(...)` calls the base class's implementation.

```flywheel
class Animal
    string name
    function speak() returns string
        return "..."
    end
    function introduce() returns string
        return "{inst name} says {inst speak()}"
    end
end
class Dog extends Animal
    function speak() returns string
        return "Woof"
    end
end
Animal pet = Dog("Rex")
print pet.introduce()   # Rex says Woof
```

Method calls dispatch on the receiver's runtime class whenever subclasses or implementations differ; when every possible class uses the same method the call is direct. Because the whole program is compiled together, the set of possible classes is known statically. A subclass value converts implicitly to its base class or interface (including nullable forms). Redeclaring an inherited field is an error, and inheritance cycles are rejected.

If a base class defines `init` and the subclass does not, the subclass inherits it only when it adds no required fields.

## Interfaces

An interface lists method signatures. `class Circle implements Shape, Named` must provide (or inherit) every method with a matching signature. Interfaces may extend other interfaces. Values of interface type dispatch to the implementing class; interfaces cannot be constructed.

```flywheel
interface Shape
    function area() returns double
end
class Square implements Shape
    double side
    function area() returns double
        return inst side * inst side
    end
end
Shape[] shapes = [Square(2.0)]
print shapes[0].area()
```

## Type tests and casts

`value is Type` tests the runtime class of a class or interface value (null is never an instance). `value as Type` is a checked cast: upcasts are free, downcasts raise `CastError` on mismatch, and `as Type?` yields null instead. Casting between unrelated classes is a compile error. For non-class types, `x is T` is decided statically; for a nullable primitive it tests for presence.

Conditions narrow variables. Inside `if pet is Dog then`, `pet` has type `Dog`. A narrowed local cannot be reassigned in that block. A narrowed module global is re-checked on each read, because a call could change it.

## Enums

```flywheel
enum Color
    Red, Green
    Blue
end
var c = Color.Green
print c, c.name, c.value    # Green Green 1
print Color.values()        # [Red, Green, Blue]
print Color.from_name("Blue") ?? Color.Red
```

Members are separated by commas or newlines. Enum values are compact integers (`.value` is the member index), print as their member name, support `==` and `!=`, and can be map keys, set elements, and JSON values (encoded by name). `int(c)` converts explicitly. `Color.from_name(text)` returns `Color?`.

### Enums with data

A member can carry fields, written like a parameter list. Such an enum is a family of immutable values, one shape per member:

```flywheel
enum Shape
    Circle(double radius)
    Rect(double width, double height)
    Empty
end

function area(Shape s) returns double
    match s
        case Shape.Circle(r) then
            return 3.14159 * r * r
        case Shape.Rect(w, h) then
            return w * h
        case Shape.Empty then
            return 0.0
    end
end

Shape[] shapes = [Shape.Circle(1.0), Shape.Rect(2.0, 3.0), Shape.Empty]
print shapes[1], Shape.Circle(2.0).radius   # Shape.Rect(width=2, height=3) 2
```

`Shape.Circle(1.0)` constructs a member (fields may have defaults); a member without fields is written as a value, `Shape.Empty`. The enum itself cannot be constructed. Fields are `readonly` and readable by name on a value whose static type is the member (`Shape.Circle(2.0).radius`, or after `case Shape.Circle c`). Members print as `Shape.Circle(radius=1)`, and `==` compares the member and its field values (deeply, through nested enum values, arrays, and tuples). A `match` on the enum is exhaustive when every member is covered. Internally the enum is a base class and each member a subclass, so `case Shape.Circle c` and `s is Shape.Circle` also work. Enums with data are not hashable, so they cannot be map keys or set elements.

## Pattern matching

`match` compares one value against a sequence of cases:

```flywheel
function describe(Shape shape) returns string
    match shape
        case Circle c if c.radius > 10.0 then
            return "big circle"
        case Circle c then
            return "circle"
        case Square then
            return "square"
    end
end

match code
    case 0 then
        print "ok"
    case 1, 2 then
        print "warning"
    case 3..9 then
        print "error"
    else
        print "unknown"
end
```

Case patterns:

| Pattern | Matches |
|---|---|
| `case 1, 2, "x"` | equal values (any expression); several values separated by commas |
| `case 1..5` / `case 1..<5` | numbers or chars in the inclusive or half-open range |
| `case Color.Red` or `case Red` | an enum member (unqualified names work when the subject is an enum) |
| `case null` | null (nullable subjects) |
| `case Circle c` / `case Circle` | instances of a class or interface; `c` is bound with that type |
| `case int n` | the present value of a nullable subject such as `int?` |
| `case ok v` / `case error e` | the variants of a `Result<T,E>`, binding the payload |
| `case Shape.Circle(r)` | a member of an enum with data, binding (or comparing) its fields in order |
| `case [x, y]` | a tuple or array with exactly that many elements, binding (or comparing) them |

In the destructuring patterns, a bare name binds that element, `_` skips it, any other expression is compared with `==`, and patterns nest: `case [Shape.Rect(w, _), 0] then`. A tuple pattern whose parts all bind always matches tuples of that length; an array pattern also checks the length.

### Destructuring declarations

`var (a, b) = pair` and `var [x, y] = items` declare one name per element of a tuple or array (`const` works too, and `_` skips an element). The number of names must equal the number of elements: tuples are checked by the compiler, arrays raise `ValueError` when the lengths differ.

```flywheel
function min_max(int[] xs) returns [int, int]
    var sorted = xs.copy()
    sorted.sort()
    return [sorted[0], sorted[sorted.length - 1]]
end
var (low, high) = min_max([3, 1, 2])
```

Any case may add a guard: `case Circle c if c.radius > 1.0 then`. The subject is evaluated once. Cases are tried in order, and only the first matching case runs; there is no fallthrough.

Without `else`, a `match` must be exhaustive. The compiler checks enum members, `true`/`false`, `ok`/`error`, `null` plus a type pattern for nullable values, and every concrete class that can have the subject's static type. Guarded cases do not count toward exhaustiveness. A non-exhaustive match is a compile error naming the missing cases.

## Generics

Classes and top-level functions can declare type parameters:

```flywheel
class Box<T>
    T value
    function map_to_string(function(T) returns string f) returns string
        return f(inst value)
    end
end
function first<T>(T[] items) returns T
    return items[0]
end

var b = Box(5)                 # Box<int>, inferred from the argument
Box<string> s = Box("hi")      # type arguments can also come from the declared type
print first([1, 2]), first<double>([1.5])
var pick = first<string>       # an instantiated generic function as a value
```

Generics are monomorphized: every distinct set of type arguments compiles a separate copy, so generic code runs as fast as hand-written code. Type arguments are written directly after the name with no space (`Box<int>`); with a space, `a <b` is a comparison or markup. Arguments are inferred from call arguments (unifying parameter types such as `T[]`, `map<K, V>`, `T?`, and function types) and, for constructors, from the expected type. If a parameter cannot be inferred, write the arguments explicitly.

Generic bodies are type-checked when instantiated, like C++ templates; a generic that is never used is not checked.

### Constraints

A type parameter can require an interface or base class (`T implements Named`, `T extends Shape`) or a built-in capability: `T is numeric`, `T is comparable` (numbers, `string`, `char`, or classes with `operator <`), or `T is hashable` (valid map keys). A type argument that does not satisfy the constraint is reported at the call or type, with the constraint, instead of somewhere inside the generic body:

```flywheel
function largest<T is comparable>(T[] items) returns T
    var best = items[0]
    for x in items do
        if x > best then
            best = x
        end
    end
    return best
end
```

### Generic methods

Methods can declare their own type parameters, inferred from the arguments like generic functions:

```flywheel
class Box<T>
    T value
    function map<U>(function(T) returns U f) returns Box<U>
        return Box<U>(f(inst value))
    end
end
print Box(4).map(function(int x) => "n{x}").value   # n4
```

Each instantiation is a separate non-virtual method: a subclass cannot override a generic method.

## Optional values

Any type can be made nullable with `?`: `int?`, `double?`, `bool?`, `char?`, `Color?`, `Person?`, `string[]?`.

```flywheel
int? port = null
print port ?? 8080          # ?? supplies a fallback when the left side is null
if port != null then
    print port + 1          # narrowed to int
end
function find(int[] xs, int x) returns int?
    for i, item in xs do
        if item == x then
            return i
        end
    end
    return null
end
```

Nullable primitives store present values in a small heap box, so `int?` costs an allocation; `null` costs nothing. `==` and `!=` compare presence and value (`a == 5` is false for null). Arithmetic and ordering need non-null operands: check for null, use `??`, or cast with `int(x)` (which raises `CastError` for null).

Null checks narrow variables in `if`, `while`, and the right side of `and`/`or`: `if x != null then`, `while item != null do`, and `x != null and x > 0`. After `if x == null then return end` (an early exit), the rest of the block sees `x` as non-null. Narrowed reads are null-checked, so narrowing stays sound even if the variable is reassigned. Member access on a nullable class value is also checked automatically.

`a ?? b` evaluates `b` only when `a` is null. The result is non-null unless `b` is nullable too.

## Characters and strings

`char` is a Unicode scalar value, written `'a'`, `'é'`, `'\n'`, or `'\u{1F600}'`. Chars compare with `== != < <= > >=`, convert explicitly with `int(c)`, `char(65)`, `char("x")`, and `string(c)`, and concatenate with strings (`"a" + 'b'`). `s[i]` returns a `char`; `for c in text` iterates characters; `'x' in text` tests membership.

Strings are UTF-8, and every length and index counts characters (Unicode scalar values), not bytes: `"héllo".length` is 5. String methods are listed in the [library reference](../stdlib/README.md#strings).

Identifiers may use non-ASCII letters (`var größe = 3`). Punctuation, symbol, and emoji blocks are rejected. Names are compared by exact UTF-8 bytes; there is no Unicode normalization.
