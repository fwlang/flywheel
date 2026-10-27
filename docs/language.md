# Implemented grammar decisions

The specification remains the source of truth. This document records implemented provisional choices and current restrictions; it does not redefine the full target.

## Precedence (lowest to highest)

| Level | Operators | Associativity |
|---|---|---|
| 1 | `or` | left, short circuit |
| 2 | `and` | left, short circuit |
| unary | `not` | takes comparison/equality expression, binds above `and` |
| 3 | `== !=` | left |
| 4 | `< <= > >= in` | left |
| 5 | `+ -` | left |
| 6 | `* / // %` | left |
| unary | `+ -` | right |
| postfix | calls, indexing, member access | left |

Comparisons do not chain: use `a < b and b < c`. Assignment is a statement. No implicit truthiness is allowed. Arithmetic evaluation is left to right. Integer `/` produces binary64, explicitly rounding operands as part of division; floating `/` preserves the common floating type. Integer `//` truncates toward zero, and `%` follows the dividend's sign. Division by zero and signed minimum divided/remaindered by -1 trap. Integer overflow is checked at all optimization levels.

Implicit conversions permit only lossless type-wide widening. A constant integer initializer fitting its destination integer type is also permitted. In particular, reading an `int` variable cannot implicitly convert it to `double`. Explicit numeric casts check range, truncate fractional real-to-integer values toward zero, and may round representable integer-to-real values. Narrow real casts reject out-of-range values. Unsigned literals larger than signed 64-bit maximum are not yet supported. Nullable reference types are supported; nullable primitive layouts are pending.

Newlines terminate statements except inside parentheses/brackets/braces or a markup token. Semicolons separate statements at the top grouping level. There is no backslash continuation syntax yet. UTF-8 source is validated, strings are UTF-8, and identifiers are restricted to ASCII. There is no Unicode identifier normalization in this checkpoint. String `length` and `len` count UTF-8 **bytes**, not Unicode scalars.

`f(a,b)` is canonical. Outermost statement/declaration/assignment calls can omit parentheses. Nested calls must be parenthesized. Known named functions and `print` accept signed outer arguments such as `print -7`; ambiguous member/function-valued calls with unary signs should use parentheses. Bare function names and member callable statements invoke zero-argument functions. Function references can be assigned and called locally, but callable parameter type syntax, signature-based reassignment between different functions, and closures are missing.

Arrays infer a homogeneous common type, or use the declared element type. Empty arrays require context. Heterogeneous tuple inference is diagnosed as unimplemented; tuple/map syntax and growable collection methods are still missing. Arrays use shared mutable identity and checked indices. `for` creates a fresh lexical binding and evaluates the iterable once; only arrays are currently iterable.

Generated class constructors follow field order, accept defaults, and require all missing non-default fields. Defaults resolve against module scope. User-defined `init` is explicitly rejected until definite field initialization is implemented. Explicit return types support recursion; recursive inferred returns require an annotation. Non-void functions require returns on all paths; infinite-loop return analysis is conservative.

Markup supports named tags, self-closing tags, and fragments. `</>` only closes a fragment. Indentation-only text containing a newline between children is discarded; other text whitespace is retained. Entities supported: `amp`, `lt`, `gt`, `quot`, `apos`, decimal numeric, and hexadecimal numeric. HTML names require `import html.*`; the implemented tag and attribute sets are intentionally partial. `data-*` and `aria-*` are the documented attribute extension mechanism. Numeric attributes currently require explicit `string(...)`. Native browser event callbacks are rejected.

Components specialize from supplied concrete attribute types and must use each referenced attribute consistently across uses. `style` defaults to empty string and `inner` to an empty fragment. Other referenced attributes are required. Attribute errors are checked when a component is instantiated; unused templates do not yet receive complete semantic checking. Generic element-family declarations, a complete element inspection API, and constant-template lowering remain pending.

Imports currently recognize native builtin modules, aliases, and `html.*`. Local `.fly` resolution, other wildcard exports, packages, and cycle diagnostics are not yet implemented. This restriction is an outstanding deviation from the specified local-first resolution rule.
