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

Native libraries (`core`, `collections`, `compress`, `crypto`, `desktop`, `encoding`, `fs`, `gc`, `html`, `io`, `json`, `math`, `net`, `os`, `regex`, `sqlite`, `svg`, `testing`, `thread`, `time`, `toml`, `uuid`, `web`, and `yaml`) and library modules written in Flywheel (`args`, `bigint`, `csv`, `decimal`, `log`, and `url`) support qualified imports and aliases; see the [library reference](../stdlib/README.md). Native wildcard imports apply to the tag sets `html.*` and `svg.*`; local modules support wildcard imports for all their public declarations. [Element families](html-dom.md#element-families) declared in a module are visible in that module and in modules that import it. Generic classes and functions can be used across modules with type arguments, such as `shapes.Box<int>`. Packages are installed by [Pulley](tooling.md#packages); explicit re-export syntax is future work.

A local module named like a native library takes precedence, so a file `gc.fw` next to the entry file replaces `import gc`.

Run the multi-file example with:

```sh
build/flywheel run examples/modules/main.fw
```
