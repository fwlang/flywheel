# Native library API at this checkpoint

Libraries are currently compiler-resolved declarations backed by C++ runtime functions. No library function uses a Flywheel interpreter. This is a partial implementation, not the complete standard library.

| Module | Implemented API |
|---|---|
| implicit core / `core` | `print(value)`; unqualified `typeof(value)`, `len(string_or_array)`, numeric casts, `string(value)`, `render(tree)` |
| `html` | subset of HTML tags through `import html.*`; `html.render(element_or_fragment)` |
| `fs` | `exists(string) returns bool`, `read(string) returns string`, `write(string,string) returns void`, `remove(string) returns void` |
| `math` | `PI`, `sqrt(double)`, `sin(double)`, `cos(double)`, `pow(double,double)`; results are `double` |
| `os` | `env(string) returns string`; missing environment variable terminates with an error |
| `time` | `monotonic() returns double` in seconds; `sleep(double)` in seconds |
| `json` | `parse(string)`, `stringify(value)`, `kind(value)`, `get(value,key)`, `has(value,key)`, `as_string(value)`, `as_int(value)`, `as_double(value)`, `as_bool(value)` |
| `web` | `serve(string route, handler, int port)`; handler has no arguments and returns `element` or `fragment` |
| `testing` | `assert(bool, optional_string_message)` |

`json.parse` produces an inferred opaque `json.value`. `get` currently selects an object member, not an array index. `kind` returns `null`, `bool`, `number`, `string`, `array`, or `object`. Number tokens preserve their decimal spelling; `as_int` requires an integer in the signed 64-bit range. Parsing handles nested objects/arrays, escaped Unicode and surrogate pairs, and limits nesting to 256. Duplicate keys, malformed data, wrong scalar types, and missing keys are errors. `stringify` emits objects in sorted-key order. Generic Flywheel-value serialization and JSON array access are pending.

Every generated class has `Class.from_json(string)` for scalar string, numeric, and bool fields. Missing defaulted fields use their declaration defaults. Missing required fields and invalid types fail at runtime. Extra JSON fields are currently ignored. Nested class fields, nullable fields, and array fields are not yet supported by this generated conversion.

`web.serve` blocks and serves one route over IPv4 loopback. GET returns UTF-8 HTML, unknown routes return 404, other methods return 405, and connections close after each response. Query strings are excluded from route matching. Requests are bounded to a 16 KiB header read and use socket timeouts. There is no request body API, persistent connection support, multiple-route registry, worker concurrency, TLS, or async I/O yet.

`io`, `net`, and `collections` are not implemented. The remaining APIs in all partial modules, typed `FileError`, other exceptions, and `Result<T,E>` remain work items. Library errors currently report to stderr and exit with status 1.
