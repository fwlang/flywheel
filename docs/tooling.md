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

Local variable and type information is not emitted yet, so debuggers show source positions but not Flywheel variable values.

## Editor integration

`flywheel lsp` is a Language Server Protocol server over standard input and output. It provides:

- diagnostics on open, change, and save, checking unsaved buffers (including unsaved imported modules that are open in the editor) with the full compiler front end; and
- whole-document formatting with the formatter above.

Configure an editor's generic LSP client to start `flywheel lsp` for `.fw` and `.fly` files. The VS Code extension in `fw-vscode` currently provides syntax highlighting and snippets; it does not yet launch the server. The compiler reports the first error it finds, so the server publishes one diagnostic at a time.
