# Implemented grammar decisions

This document records implemented provisional choices and current restrictions. Classes, inheritance, interfaces, enums, pattern matching, generics, optional values, and characters are described in [types](types.md).

## Precedence (lowest to highest)

| Level | Operators | Associativity |
|---|---|---|
| 1 | `or` | left, short circuit |
| 2 | `and` | left, short circuit |
| unary | `not` | takes comparison/equality expression, binds above `and` |
| 3 | `== !=` | chained |
| 4 | `< <= > >= in is` | chained (`in` and `is` do not chain); strings order by code point |
| 5 | `??` | left; the right side is evaluated only when needed |
| 6 | `.. ..<` | left (integer endpoints) |
| 7 | `+ -` | left |
| 8 | `* / // %` | left |
| 9 | `as` | postfix checked cast |
| unary | `+ -` | right |
| postfix | calls, indexing, member access | left |

Comparisons chain: `a < b <= c` means `a < b and b <= c`, evaluating `b` once and stopping at the first false comparison. Equality operators chain among themselves (`a == b == c`); mixing levels does not chain, so `a == b < c` is `a == (b < c)`. Assignment is a statement. No implicit truthiness is allowed. Arithmetic evaluation is left to right.

Integer `/` produces binary64, explicitly rounding operands as part of division; floating `/` preserves the common floating type. `//` is integer division for any numeric operands and always returns `int`: integers divide exactly, and floating operands divide and truncate toward zero (`7.5 // 2.0` is `3`, `-7.9 // 2` is `-3`), matching `%`, which follows the dividend's sign. `//=` is the compound form. Division by zero (including floating `//` by zero) and signed minimum divided/remaindered by -1 trap. Integer overflow is checked at all optimization levels.

Implicit conversions permit only lossless type-wide widening. A constant integer initializer fitting its destination integer type is also permitted. In particular, reading an `int` variable cannot implicitly convert it to `double`. Explicit numeric casts check range, truncate fractional real-to-integer values toward zero, and may round representable integer-to-real values. Narrow real casts reject out-of-range values. Integer literals above the signed 64-bit maximum (up to 18446744073709551615) have type `u64`.

## Lines and tokens

Newlines terminate statements except inside parentheses/brackets/braces or a markup token. A line also continues onto the next one when it ends with a backslash, a binary operator, a comma, `=`, or `=>`:

```flywheel
int first = 1
int second = 2
var total = first +
    second +
    3
var other = first \
    + second
```

Semicolons separate statements at the top grouping level. UTF-8 source is validated and strings are UTF-8. Identifiers may contain non-ASCII letters; see [characters and strings](types.md#characters-and-strings). String `length` and `len` count characters (Unicode scalar values), not bytes. `'x'` is a `char` literal; `{name}` inside a string interpolates, but a `{` followed by whitespace is a literal brace, as is a `}` with whitespace on either side, so `"{ name }"` is the text `{ name }` and CSS or JavaScript such as `"p { color: {c}; }"` reads naturally (the `{c}` still interpolates). The same rule applies to markup text. Write `{{` / `}}` for a literal brace with no space beside it, as in compact JSON.

## Calls and construction

`f(a,b)` is canonical. Outermost statement/declaration/assignment calls can omit parentheses, including markup arguments such as `print <h1>Hello</h1>` and `var tree = identity <p>Hello</p>`. Nested calls must be parenthesized. Known named functions and `print` accept signed outer arguments such as `print -7`; ambiguous member/function-valued calls with unary signs should use parentheses. Bare function names and member callable statements invoke zero-argument functions. `print a, b, c` prints its values separated by spaces.

A bare class name is a class reference; a call constructs. Both `var window = desktop.window "my-window"` and `var window = desktop.window("my-window")` create a window, `desktop.window()` creates one with defaults, and `var Window = desktop.window` stores the class itself, which can later be called (`Window()`, `Window "title"`). This applies equally to user classes and through module aliases. See [classes and constructors](types.md#classes-and-constructors).

Generated class constructors follow field order, accept defaults, and require all missing non-default fields. Classes can define `init` instead. Defaults resolve against module scope. Explicit return types support recursion; recursive inferred returns require an annotation. Non-void functions require returns on all paths; infinite-loop return analysis is conservative.

## Functions, lambdas, and methods

Function types are written `function(int, string) returns bool`; `function() returns void` takes nothing. They work for parameters, returns, fields, locals, and collection elements. A suffix after a function type belongs to its return type, so `function() returns int[]` returns an array; parentheses group a function type to apply the suffix to the function itself: `(function(int) returns int)[]` is an array of functions (also written `array<function(int) returns int>`), and `(function() returns int)?` is a nullable function. A named top-level function converts to a matching function type; mismatched signatures are diagnosed. Lambdas have an expression form, `function(int x) => x * 2` (result type inferred), and a block form ending in `end`, optionally with `returns T`. Lambda parameters must be typed and cannot have defaults; block bodies with a non-void result must return on all paths.

Methods are first-class: `obj.method` (and `inst method` inside a class) is a closure bound to that receiver, with the method's function type, and dispatches like a call. Class references convert to constructor functions.

```flywheel
class Counter
    int value = 0
    function add(int n) returns int
        inst value += n
        return inst value
    end
end
var counter = Counter()
var add = counter.add
print [1, 2, 3].map(counter.add)
```

Lambdas capture enclosing locals and parameters. A variable that some lambda captures and that is assigned anywhere (inside or outside a lambda) is stored in a shared heap cell, so every closure and the declaring scope see the same, current value:

```flywheel
function counter() returns function() returns int
    int count = 0
    return function() returns int
        count += 1
        return count
    end
end
```

Variables that are captured but never assigned are copied into the closure, which is cheaper. Each `for` iteration has a fresh binding, so closures created in a loop see their own iteration's value. Captured references share identity. Closures are called like functions, `f(x)` or `f.call(x)`, including field closures such as `button.on_click()`. Arrays provide `map`, `filter`, `reduce(initial, f)`, `any`, `all`, `each`, and `sort(less)`.

Zero-argument function and method declarations may omit `()`: `function main returns void`, `function answer returns int`, or `function inferred` followed by its body. Non-empty parameter lists still require parentheses and typed parameters.

## Format specs

An interpolation can end with `:` and a format spec, as in Python: `"{price:.2f}"`, `"{n:>8}"`, `"{count:,}"`. The spec is `[[fill]align][sign][#][0][width][,|_][.precision][type]`:

| Part | Meaning |
|---|---|
| align | `<` left, `>` right, `^` center, `=` pad after the sign; the optional fill before it is any character (`{x:*^9}`) |
| sign | `+` always, `-` only for negatives (default), space for a leading space on positives |
| `#` | `0x`/`0o`/`0b` prefixes for `x`, `o`, `b` |
| `0` | pad numbers with zeros after the sign (`{n:05}` is `00042`) |
| width | minimum width in characters |
| `,` or `_` | group thousands (`_` groups hexadecimal and binary digits by four) |
| `.precision` | digits after the point for `f`, `e`, `%`; significant digits for `g` or no type; maximum characters for text |
| type | `d` integer, `f` fixed, `e` exponent, `g` general, `%` percent, `x`/`X` hex, `o` octal, `b` binary, `c` character, `s` text |

Integers accept the floating types too (`{n:.2f}`), and floats accept the integer types when they hold a whole number. Other values are formatted as text first, so width, alignment, and precision apply to anything. An invalid spec raises `ValueError`. A `:` inside brackets, braces, or quotes does not start a spec, so `{m["a:b"]}` and `{ {"k": 1} }` are unaffected.

```flywheel
var price = 1234.5
print "{price:,.2f} | {42:>6} | {0.256:.1%} | {255:#x} | {"ok":*^6}"
# 1,234.50 |     42 | 25.6% | 0xff | **ok**
```

## Cleanup with defer

`defer statement` runs the statement when the enclosing block ends, however it ends: normally, by `return`, `break`, or `continue`, or by an error. Several `defer`s in one block run in reverse order. A deferred block is written `defer` ... `end`.

```flywheel
import io

function copy_first_line(string from, string to)
    var input = io.open(from)
    defer input.close()
    var output = io.open(to, "w")
    defer output.close()
    output.write(input.read_line() ?? "")
end
```

The block is the enclosing `if`, loop, `try`, or function body, so a `defer` inside a loop runs at the end of each iteration. `defer` is not allowed at the top level of a file.

## Collections and control flow

Arrays infer a homogeneous common type, or use the declared element type. Empty arrays require context. Heterogeneous literals infer tuples, with compile-time integer indices. Maps use `{key: value}` literals and sets `{a, b}`; `{}` is an empty map or set, depending on the expected type. See [collections](collections.md). Arrays use shared mutable identity and checked indices; `s[a..b]` and `s[a..<b]` slice strings and sequences.

`for` creates a fresh lexical binding and evaluates the iterable once. Arrays, tuples, strings (by character), maps (by key), sets, and classes with an iterator protocol are iterable. `for key, value in map` and `for index, item in array` bind two names.

## Markup

Markup supports named tags, self-closing tags, and fragments. Attribute values are written as in HTML: `class="card"`, `class='card'`, unquoted `class=card` (no spaces, quotes, `=`, `<`, `>`, backticks, or braces), or `{expression}`; quoted values may interpolate (`class="item {kind}"`). An attribute without a value, such as `<input disabled>`, is an empty string, or `true` for HTML boolean attributes (`disabled={flag}` sets one conditionally). HTML void elements (`br`, `hr`, `img`, `input`, `link`, `meta`, ...) need no `/>` or closing tag. `</>` only closes a fragment. Indentation-only text containing a newline between children is discarded; other text whitespace is retained. HTML comments (`<!-- ... -->`) may appear among children; they are dropped at compile time, as `html.parse` drops them, and their contents are not interpolated. Text on both sides of a comment joins. Entities supported: `amp`, `lt`, `gt`, `quot`, `apos`, decimal numeric, and hexadecimal numeric. HTML names require `import html.*`, SVG names `import svg.*`, and other tag sets can be declared as [element families](html-dom.md#element-families). `data-*` and `aria-*` attributes are accepted everywhere. Numeric attribute values render with their usual text. `on*` attributes take inline JavaScript text, or a Flywheel function that runs in [desktop windows](desktop.md).

Components specialize from supplied concrete attribute types and must use each referenced attribute consistently across uses. `style` defaults to empty string and `inner` to an empty fragment. Other referenced attributes are required. Attribute types are checked when a component is instantiated; components that are never used still have their tags, attribute names, and void-element children checked.

Imports resolve local `.fw` and `.fly` modules before native modules, supporting aliases, local wildcard exports, public/private declarations, and import-cycle diagnostics. See [module resolution](modules.md).

Elements and fragments format as escaped HTML in `print`, `string(value)`, and string interpolation. Explicit `html.render(tree)` remains available. Formatting returns an ordinary string; inserting that string into markup escapes it as text, while inserting an element directly preserves its node structure. An `element` binding accepts a fragment as a renderable sequence, and both expose `.render()`.

Typed exceptions, `finally`, and `Result<T,E>` are described in [error handling](errors.md). Explicit casts and nested collection formatting, homogeneous tuples, repetition and ranges are described in [value conversions](value-conversions.md). HTML tree querying and mutation are described in [HTML tools](html-dom.md). Markup tag resolution uses component/tag names independently of ordinary variables and functions, so `var div = <div />` remains valid.
