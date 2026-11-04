# Modules and visibility

A module is one source file. Both `.fw` and `.fly` are supported. Compile the application's entry file; imports are resolved, checked, and included in the native executable automatically.

```flywheel
import helpers
import helpers as utils
import widgets.*
import package.formatting as formatting
```

`import helpers` binds a namespace; use `helpers.add(2, 3)`, `helpers.counter`, or `helpers.Person("Danny")`. Module-qualified types work in declarations and signatures, for example `helpers.Person user = helpers.Person("Danny")`. Components can be qualified, such as `<widgets.Title>Hello</widgets.Title>`, or imported by wildcard.

Resolution searches the importing file's directory first, then the entry file's directory. Dots become directory separators (`package.formatting` means `package/formatting`). Within each directory, `.fw` is tried before `.fly`. Next come installed packages (see [packages](tooling.md#packages)): the nearest `.flywheel-modules` directory, then the Flywheel installation's `modules` directory. `import shapes` loads the main module of the `shapes` package and `import shapes.geometry` its `geometry.fw`. Then library modules (`csv`, `url`, `log`, `args`, `bigint`, `decimal`, which are written in Flywheel and built into the compiler) and native modules. A local file takes precedence over a package or library module with the same name. Imports bind the last path segment unless an `as` alias is provided.

Modules are identified by canonical file path and included once, even when imported through multiple aliases or dependencies. Initialization runs in dependency order, preserving source import order among dependencies. Import cycles are rejected with a diagnostic showing the path chain. Reading a module global through a function before its initializer completes raises `RuntimeError`, preventing access to an uninitialized reference. Independent modules have separate symbol namespaces. Imported names are resolved in their defining module; caller locals cannot change a function's or field's default expression bindings.

## String, path, and URL imports

A module name can be written as a string: `import "helpers"` is `import helpers`, and `import "package.formatting"` is `import package.formatting`. A string is the way to import a module whose name is a keyword of the import statement, such as `import "c"`.

A string can also name a file or a URL. Relative paths start at the importing file's directory (then the entry file's directory); the extension may be left out. The module is bound to its file name, or to an `as` alias, which is required when the file name is not an identifier:

```flywheel
import "lib/strings.fw"                       # bound as strings
import "../shared/util" as util
import "https://example.com/fw/colors.fw"
import "https://example.com/fw/my-lib.fw" as mylib
```

URLs use the schemes of [source URLs](tooling.md#source-code-and-urls) (`https`, `http`, `ftp`, `ftps`, `sftp`, `ssh`, `scp`, `file`). A module loaded by URL imports its own relative files and sibling modules by URL as well.

## Native imports: C, C++, Rust, and Zig

`import c name`, `import cxx name` (also written `import++ name`), `import rust name`, and `import zig name` compile a source file of that language and expose its functions as a module:

```c
// geometry.c
double hypotenuse(double a, double b) { return sqrt(a * a + b * b); }
int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
```

```rust
// fast.rs
pub fn fib(n: u64) -> u64 { if n < 2 { n } else { fib(n - 1) + fib(n - 2) } }
pub fn shout(s: &str) -> String { s.to_uppercase() }
```

```flywheel
import c geometry
import rust fast
import++ "vendor/text.hpp" as text

print geometry.hypotenuse(3.0, 4.0), fast.fib(30), fast.shout("hi")
```

A dotted name (`import c native.geometry`) searches `native/geometry` next to the importing file and in the entry directory, with the language's extensions: `.c`, `.h` (C); `.cpp`, `.cc`, `.cxx`, `.c++`, `.hpp`, `.hh`, `.hxx`, `.h` (C++); `.rs` (Rust); `.zig` (Zig). A string names a path or a URL, which is downloaded. A Flywheel module named `c`, `cxx`, `rust`, or `zig` is imported with a string: `import "c"`.

Functions are exposed when their parameters and result use these types:

| Flywheel | C and C++ | Rust | Zig |
|---|---|---|---|
| `int` | `int`, `long`, `short`, `unsigned`, `size_t`, `int64_t`, ... | `i8`–`i128`, `u8`–`u128`, `isize`, `usize` | `i8`–`i64`, `u8`–`u64`, `isize`, `usize`, `c_int`, ... |
| `double` | `float`, `double` | `f32`, `f64` | `f16`, `f32`, `f64` |
| `bool` | `bool`, `_Bool` | `bool` | `bool` |
| `string` | `const char *`, `char *`; C++: `std::string`, `const std::string &`, `std::string_view` | `&str`, `String`, `&String` | `[]const u8`, `[:0]const u8`, `[*:0]const u8` |
| result only: nothing | `void` | `()` | `void` |

Functions with other types (pointers, structs, classes, templates, generics), `main`, functions inside C++ namespaces and classes, and functions named like Flywheel keywords are not exposed; only the first of overloaded C++ functions is. Rust functions must be `pub` (or `pub(crate)`) and Zig functions `pub` or `export`; C `static` functions are exposed, since the module is compiled together with its source. Integers are converted from Flywheel's 64-bit `int` with the language's usual casts. Strings are UTF-8; string results are copied.

Failures become catchable `NativeError`s: C++ exceptions (with their `what()` message), Rust panics (with the panic message), and Zig error-union results (`!T`, with the error name). Crashes in native code (null pointers, aborts) end the program.

Each module is compiled once and cached in Flywheel's cache directory (`$XDG_CACHE_HOME/flywheel` or `~/.cache/flywheel`, `%LOCALAPPDATA%\flywheel` on Windows) and recompiled when its source changes. C and C++ use the C++ compiler Flywheel was built with; Rust needs `rustc`, and Zig needs `zig` 0.12 or newer, on `PATH`. `flywheel build` links the native code into the executable; `flywheel check` only reads the sources.

## Public and private declarations

Top-level declarations are **public by default**. `public` makes this explicit, and `private` hides a name from other modules:

```flywheel
private const string prefix = "Hello"

private function format(string name) returns string
    return "{prefix}, {name}!"
end

public function greet(string name) returns string
    return format(name)
end
```

These modifiers apply to top-level functions, classes, interfaces, enums, components, and variable/constant declarations. Inside a class, `private` marks a member visible only to that class's methods; see [member visibility](types.md#member-visibility-and-readonly-fields). Private names are available throughout their own module, including its exported functions and templates. Accessing a private name through an imported namespace is a compile error. Returning a value of a private class does not make the class name public or hide the value's existing fields.

`import helpers.*` imports that module's own public declarations. Private declarations and imported namespaces are not re-exported. Collisions with another wildcard export, an import alias, or a local top-level declaration are compile errors. Exported `const` bindings remain immutable through qualified access. Mutable module globals retain shared identity across aliases.

Native libraries (`core`, `collections`, `compress`, `crypto`, `desktop`, `encoding`, `flywheel`, `fs`, `gc`, `html`, `io`, `json`, `math`, `net`, `os`, `regex`, `sqlite`, `svg`, `testing`, `thread`, `time`, `toml`, `uuid`, `web`, and `yaml`) and library modules written in Flywheel (`args`, `bigint`, `csv`, `decimal`, `log`, and `url`) support qualified imports and aliases; see the [library reference](../stdlib/README.md). Native wildcard imports apply to the tag sets `html.*` and `svg.*`; local modules support wildcard imports for all their public declarations. [Element families](html-dom.md#element-families) declared in a module are visible in that module and in modules that import it. Generic classes and functions can be used across modules with type arguments, such as `shapes.Box<int>`. Packages are installed by [Pulley](tooling.md#packages); explicit re-export syntax is future work.

A local module named like a native library takes precedence, so a file `gc.fw` next to the entry file replaces `import gc`.

Run the multi-file example with:

```sh
build/flywheel run examples/modules/main.fw
```
