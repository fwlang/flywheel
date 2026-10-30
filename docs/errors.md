# Exceptions and typed Results

`throw expression` raises a typed value. `catch Type name` matches its type; `catch name` or `catch Error name` catches any error. A catch-all must be last. Primitive values, strings, class instances, and supported collection values can be thrown; null, void, and callable values cannot.

```flywheel
import fs

try
    var contents = fs.read("config.json")
    print contents
catch FileError error
    print "Read failed: {error.message}"
finally
    print "Finished"
end
```

Native runtime errors include `FileError`, `IndexError`, `KeyError`, `NumericError`, `AssertionError`, `JsonError`, `CastError`, `SelectorError`, `InputError`, `ProcessError`, `DesktopError`, `RegexError`, `NetworkError`, `ValueError` (invalid arguments and formats: format specs, destructuring lengths, frozen collections, parsing), `HttpError` (HTTP client failures), `DatabaseError` (SQLite), `ResultError`, and `RuntimeError`. They expose `.message` and format as their message. Error constructors accept a string, for example `throw FileError("missing")`. Custom nominal class errors are matched exactly. `Error` is the universal catch type; builtin errors can also be assigned to an `Error` binding. Rethrowing a catch-all error preserves its original dynamic type.

`defer` (see [language](language.md#cleanup-with-defer)) registers cleanup that runs like `finally` when its block ends.

Errors propagate across native generated function calls, out of callbacks (sort comparators, desktop handlers, web handlers, map key `hash`/`equals`), and from a thread task to the thread that calls `join`. `finally` executes on normal completion, handled or unhandled errors, returns, and loop exits. A new return, throw, break, or continue from a finalizer supersedes the pending exit. A finalizer cannot see locals declared inside its try or catch bodies. Return expressions are evaluated before finalization and their referenced objects remain alive until the return completes or is abandoned.

Generated code checks a thread-local error channel after fallible native/runtime calls. C++ runtime exceptions are caught at the C ABI boundary and do not unwind through generated functions. Failed paths release crossed lexical scopes and temporary references; function propagation drops the ownership frame. Uncaught errors print to stderr and exit with status 1. Unbounded or too-deep recursion overflows the native stack; this is reported as `Flywheel runtime error: stack overflow` and exits with status 1, but it cannot be caught. Process termination and resource-exhaustion failures are not a recoverability guarantee.

## Result<T,E>

Results are explicit tagged values, separate from the exception channel:

```flywheel
function divide(int n, int d) returns Result<int, string>
    if d == 0 then
        return Result.error("division by zero")
    end
    return Result.ok(n // d)
end

var result = divide(42, 2)
if result.is_ok then
    print result.unwrap()
else
    print result.error()
end
```

`Result.ok(value)` and `Result.error(value)` use an expected `Result<T,E>` type from a declaration, assignment, parameter, or annotated return. Without that context the compiler cannot infer the other variant and requests a type annotation. `.is_ok` / `.is_ok()` inspect the tag; `.is_error()` tests the error variant. `.unwrap()` and `.error()` retrieve their respective payloads and raise `ResultError` on the wrong variant. `match` handles both variants with `case ok value` and `case error e` and checks that both are covered. Payload ownership follows normal ARC rules.

`fs.read(path)` throws `FileError`. The separate `fs.try_read(path)` returns `Result<string,FileError>` for file failures; it does not silently change the semantics of `fs.read`. Other failures still propagate as exceptions. There is no automatic conversion between arbitrary thrown exceptions and Result values.
