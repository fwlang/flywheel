# Implemented grammar decisions

This document records implemented provisional choices and current restrictions; it does not define the full target language.

## Precedence (lowest to highest)

| Level | Operators | Associativity |
|---|---|---|
| 1 | `or` | left, short circuit |
| 2 | `and` | left, short circuit |
| unary | `not` | takes comparison/equality expression, binds above `and` |
| 3 | `== !=` | left |
| 4 | `< <= > >= in` | left |
| 5 | `.. ..<` | left (integer endpoints) |
| 6 | `+ -` | left |
| 7 | `* / // %` | left |
| unary | `+ -` | right |
| postfix | calls, indexing, member access | left |

Comparisons do not chain: use `a < b and b < c`. Assignment is a statement. No implicit truthiness is allowed. Arithmetic evaluation is left to right. Integer `/` produces binary64, explicitly rounding operands as part of division; floating `/` preserves the common floating type. Integer `//` truncates toward zero, and `%` follows the dividend's sign. Division by zero and signed minimum divided/remaindered by -1 trap. Integer overflow is checked at all optimization levels.

Implicit conversions permit only lossless type-wide widening. A constant integer initializer fitting its destination integer type is also permitted. In particular, reading an `int` variable cannot implicitly convert it to `double`. Explicit numeric casts check range, truncate fractional real-to-integer values toward zero, and may round representable integer-to-real values. Narrow real casts reject out-of-range values. Unsigned literals larger than signed 64-bit maximum are not yet supported. Nullable reference types are supported; nullable primitive layouts are pending.

Newlines terminate statements except inside parentheses/brackets/braces or a markup token. Semicolons separate statements at the top grouping level. There is no backslash continuation syntax yet. UTF-8 source is validated, strings are UTF-8, and identifiers are restricted to ASCII. There is no Unicode identifier normalization in this checkpoint. String `length` and `len` count UTF-8 **bytes**, not Unicode scalars.

`f(a,b)` is canonical. Outermost statement/declaration/assignment calls can omit parentheses, including markup arguments such as `print <h1>Hello</h1>` and `var tree = identity <p>Hello</p>`. Nested calls must be parenthesized. Known named functions and `print` accept signed outer arguments such as `print -7`; ambiguous member/function-valued calls with unary signs should use parentheses. Bare function names and member callable statements invoke zero-argument functions. Function references can be assigned and called locally, but callable parameter type syntax, signature-based reassignment between different functions, and closures are missing.

Arrays infer a homogeneous common type, or use the declared element type. Empty arrays require context. Heterogeneous literals infer tuples, with compile-time integer indices. Maps use `{key: value}` literals and contextual `map<K,V>` types; arrays support growth and removal. See [collections](collections.md). Arrays use shared mutable identity and checked indices. `for` creates a fresh lexical binding and evaluates the iterable once; arrays and maps are currently iterable.

Zero-argument function and method declarations may omit `()`: `function main returns void`, `function answer returns int`, or `function inferred` followed by its body. Explicit `function main()` remains valid. Non-empty parameter lists still require parentheses and typed parameters. Invocation rules are unchanged.

A bare class constructor on the outer declaration/assignment RHS invokes it with no arguments: `var person = Person` or `var window = desktop.window`. Parentheses preserve the constructor reference: `var Constructor = (Person)` or `var Window = (desktop.window)`. Stored constructor references support explicit calls and the same bare-RHS construction rule; parenthesize an alias to copy its constructor reference. This also applies through module namespaces and aliases. Required constructor fields still need arguments. Grouped standalone constructor references are not called; ordinary zero-argument function/method statements invoke even when grouped, such as `(window.run)`. Ordinary function references and instance assignments keep their existing behavior. Constructor references are specific to their class and currently support inferred bindings rather than callable type annotations or collection storage.

Generated class constructors follow field order, accept defaults, and require all missing non-default fields. Defaults resolve against module scope. User-defined `init` is explicitly rejected until definite field initialization is implemented. Explicit return types support recursion; recursive inferred returns require an annotation. Non-void functions require returns on all paths; infinite-loop return analysis is conservative.

Markup supports named tags, self-closing tags, and fragments. `</>` only closes a fragment. Indentation-only text containing a newline between children is discarded; other text whitespace is retained. Entities supported: `amp`, `lt`, `gt`, `quot`, `apos`, decimal numeric, and hexadecimal numeric. HTML names require `import html.*`; the implemented tag and attribute sets are intentionally partial. `data-*` and `aria-*` are the documented attribute extension mechanism. Numeric attributes currently require explicit `string(...)`. Native browser event callbacks are rejected.

Components specialize from supplied concrete attribute types and must use each referenced attribute consistently across uses. `style` defaults to empty string and `inner` to an empty fragment. Other referenced attributes are required. Attribute errors are checked when a component is instantiated; unused templates do not yet receive complete semantic checking. Generic element-family declarations, a complete element inspection API, and constant-template lowering remain pending.

Imports resolve local `.fw` and `.fly` modules before native modules, supporting aliases, local wildcard exports, public/private declarations, and import-cycle diagnostics. See [module resolution](modules.md) for initialization and visibility rules. External packages and explicit re-exports remain pending.

Elements and fragments format as escaped HTML in `print`, `string(value)`, and string interpolation. Both `print <h1>Hello</h1>` and `print (<h1>Hello</h1>)` print the serialized element. Explicit `html.render(tree)` remains available. Formatting returns an ordinary string; inserting that string into markup escapes it as text, while inserting an element directly preserves its node structure.

An `element` binding accepts a fragment as a renderable sequence, preserving its children without a wrapper. Both `element` and `fragment` expose `.render()`. Typed exceptions, `finally`, and `Result<T,E>` are described in [error handling](errors.md).

Explicit casts and nested collection formatting, homogeneous tuples, repetition and ranges are described in [value conversions](value-conversions.md). HTML tree querying and mutation are described in [HTML tools](html-dom.md). Markup tag resolution uses component/tag names independently of ordinary variables and functions, so `var div = <div />` remains valid.
