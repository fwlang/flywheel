# Tooling

## Testing

`flywheel test [files or directories] [-O0..3]` discovers test files named `test_*.fw` or `*_test.fw` (or `.fly`), recursively under the given directories (default: the current directory, skipping `build` and hidden directories). In each file, every top-level function whose name starts with `test_` and that takes no arguments is a test. Each test runs in turn; a failure or error is reported and the remaining tests still run.

```flywheel
import testing

function add(int a, int b) returns int
    return a + b
end

function test_add() returns void
    testing.assert_equal(add(2, 3), 5)
end

function test_upper() returns void
    testing.assert("abc".upper() == "ABC", "upper-casing")
end
```

```text
$ flywheel test
./test_math.fw
  ok   test_add
  ok   test_upper
1 test file(s), 0 failed
```

`testing.assert(condition, message)`, `testing.assert_equal(actual, expected, message)`, and `testing.fail(message)` raise `AssertionError`; any other uncaught error also fails the test. The command exits with status 1 when any test fails. The file's own top-level statements run first, as in an ordinary program. `testing.live_objects()` supports leak assertions.

### Expected errors

`testing.expect_error(ErrorType, function)` calls a function without parameters and requires it to raise `ErrorType` (or a subtype); it returns the error so its message can be checked. When nothing is raised, it fails with `AssertionError`; a different error propagates unchanged.

```flywheel
import testing

function test_parse_rejects_text() returns void
    var e = testing.expect_error(CastError, function() => int("x"))
    testing.assert(e.message.contains("integer"))
end
```

### Fixtures

Zero-argument functions with these names in a test file run around the tests:

| Function | Runs |
|---|---|
| `setup` | before each test |
| `teardown` | after each test, even when it fails (a failing teardown fails the test) |
| `setup_all` | once, before the first test |
| `teardown_all` | once, after the last test |

### Coverage

`flywheel test --coverage` counts how often each source line runs. After the tests it prints, for every non-test source file that was compiled, the covered and total lines and the line numbers never run, and writes `coverage.lcov` (LCOV format, for editors and coverage services) in the current directory. Library modules are not counted.

```text
coverage:
  shapes.fw  3/4 lines (75%)  missed: 9
  total  3/4 lines (75%); wrote coverage.lcov
```

Lines are statements: a line counts when any statement starting on it runs. Bodies of generic functions count once they are instantiated.

The compiler's own regression suite (Python, in `tests/`) runs with `flywheel selftest` or `ctest --test-dir build`.

## Formatting

`flywheel fmt file...` re-indents files in place by block structure with four spaces per level; `flywheel fmt --check file...` only reports files that would change (exit status 1). Continuation lines (open brackets, trailing operators, `\`) get one extra level, lambda bodies inside call arguments are indented once, and trailing whitespace and extra blank lines at the end of the file are removed.

The formatter only changes indentation and trailing whitespace. Lines that start inside a multi-line string or markup literal are left exactly as written, so formatting never changes program output.

## Debugging

`flywheel build -g` (and `run -g`, `check -g`) emits DWARF line tables for functions, methods, lambdas, and the program body, so native debuggers can set breakpoints by Flywheel source line and show Flywheel locations in backtraces:

```sh
flywheel build -g app.fw -o app
gdb ./app
(gdb) break app.fw:12
(gdb) run
(gdb) bt
```

Locals and parameters are described too. Numbers, `bool`, `char`, and enums show their values; strings, collections, and objects are pointers to runtime objects, which the runtime can format:

```text
(gdb) info locals
total = 10
label = 0x555555743440
(gdb) print fw_debug_format(label)
$1 = "sum"
```

## Race detection

`flywheel run --race` (also `build`, and `test`) builds the program with a data race detector. While the program runs, every read and write of an array, map, set, class instance, captured variable, or module global is checked against earlier accesses from other threads. Accesses are ordered by `thread.spawn` and `join`, mutex `lock`/`unlock`, and channel `send`/`receive`. When two threads access the same value, at least one writes, and nothing orders them, the program stops with exit status 66:

```text
flywheel: data race on global variable 'counter': a read by thread 2 conflicts with an earlier write by thread 1; nothing (a mutex, a channel, or join) orders them
```

The detector reports races that happen in the run being checked, without false positives from these synchronizations. It slows programs down and is meant for testing. Without `--race`, collection operations are still atomic and memory-safe; see [concurrency](concurrency.md#shared-state).

## Interactive sessions

`flywheel repl` reads statements, expressions, and declarations. An expression on its own is printed. Multi-line input (such as a function) continues until the block is complete. Each input is added to a session program that is compiled and run again from the start; only output produced by the new input is shown, and inputs that fail are dropped. Because earlier statements run again, side effects such as writing files or reading input repeat. `:show` lists the session, `:reset` clears it, and `:quit` ends it.

```text
fw> var x = 4
fw> function sq(int n) returns int
...     return n * n
... end
fw> sq(x)
16
```

## Documentation

`flywheel doc module.fw [-o module.html]` writes an HTML page for a module. The first comment block of the file describes the module; the `#` comment lines directly above a public function, class, interface, enum, constant, field, or method document it. Private declarations are left out. In comments, blank comment lines separate paragraphs, `` `code` `` marks code, and lines between ```` ``` ```` fences are shown as code blocks.

```flywheel
# Inventory helpers.

# Total of all counts.
function total(int[] counts) returns int
    return counts.reduce(0, function(int a, int b) => a + b)
end
```

## Packages

Packages are managed by [Pulley](../../pulley/README.md) (`pulley`, also `ppm`), a separate tool; the `flywheel` command only compiles, runs, and tests. Pulley reads a project's `pulley.toml`, resolves versions from Distrobase registries (`wednesware.org` by default) through Hydrogen, records them in `pulley.lock`, and installs the packages where the compiler looks for them:

```sh
pulley new app && cd app
pulley add wednesware.shapes        # adds shapes = "wednesware.shapes=^1.2.0" and installs it
flywheel run app.fw                 # `import shapes` finds .flywheel-modules/shapes
pulley install -g wednesware.shapes # for every program of this Flywheel installation
```

The compiler resolves `import name` (after local `.fw`/`.fly` files) in:

1. the nearest `.flywheel-modules` directory at or above the importing file, then
2. the installation-wide `modules` directory: `<install>/modules`, where `<install>` holds `bin/flywheel` (or is the executable's directory when it is not in `bin`), or `$FLYWHEEL_HOME/modules` when `FLYWHEEL_HOME` is set;

and then library and native modules. In a modules directory, a package is a directory named by its import name, or a `name.link` file containing the path of the package directory (Pulley uses these for path dependencies). `import name` loads the package's main module (`main` in its `pulley.toml`, else `name.fw`, `main.fw`, or `lib.fw`); `import name.part` loads `part.fw` in the package. Packages see each other by import name, because one project's packages share one directory.

## Editor integration

`flywheel lsp` is a Language Server Protocol server over standard input and output. It provides:

- diagnostics on open, change, and save, checking unsaved buffers (including unsaved imported modules that are open in the editor) with the full compiler front end;
- hover: the declaration's source line and its documentation comment;
- go to definition and find references for locals, parameters, functions, classes, fields, methods, enum members, and names in imported local modules, packages, and library modules (including names inside string interpolations);
- rename across open documents and the declaring file;
- completion of names in scope, keywords, module members after `module.`, and class members after `inst`;
- document symbols (the outline); and
- whole-document formatting with the formatter above.

Navigation uses a syntactic index of each file, so it works while a file has type errors; `obj.member` resolves through the classes that declare a member of that name. The compiler reports the first error it finds, so the server publishes one diagnostic at a time.

The [VS Code extension](https://github.com/fwlang/vscode-flywheel) starts the server for `.fw` and `.fly` files; its `flywheel.path` setting names the compiler. Other editors can start `flywheel lsp` from a generic LSP client.
