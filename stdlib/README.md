# Native library API at this checkpoint

Libraries are currently compiler-resolved declarations backed by C++ runtime functions. No library function uses a Flywheel interpreter. This is a partial implementation, not the complete standard library.

| Module | Implemented API |
|---|---|
| implicit core / `core` | `input(optional_string_prompt) returns string`; `print(value)` (automatically serializes elements/fragments); unqualified `typeof(value)`, `len(string_or_collection)`, checked scalar/collection casts, recursive `string(value)`, `render(tree)`; arrays, tuples, maps, and `Result<T,E>` |
| `html` | subset of HTML tags through `import html.*`; `html.render(element_or_fragment)`; `query(tree,selector)`, `query_all(tree,selector)`, `get_by_id(tree,id)` and [tree tools](../docs/html-dom.md) |
| `fs` | `exists(string) returns bool`, `read(string) returns string`, `write(string,string) returns void`, `remove(string) returns void`, `try_read(string) returns Result<string,FileError>` |
| `math` | `PI`, `sqrt(double)`, `sin(double)`, `cos(double)`, `pow(double,double)`; results are `double` |
| `os` | `env(string) returns string`, `shell(string) returns int`, `command(string[]) returns int`; see [input and processes](../docs/input-processes.md) |
| `time` | `monotonic() returns double` in seconds; `sleep(double)` in seconds |
| `json` | `parse(string)`, `stringify(value)`, `kind(value)`, `get(value,key)`, `has(value,key)`, `as_string(value)`, `as_int(value)`, `as_double(value)`, `as_bool(value)` |
| `desktop` | `window(optional_title, optional_width, optional_height)` / `Window`; `.load(element_or_fragment)`, `.run()`; see [desktop windows](../docs/desktop.md) |
| `web` | `serve(route, handler, port)`, `route(method, route, handler, port)`, `static(prefix, directory, port)` return the URL; `wait()`; `text`/`html`/`json`/`redirect(body, optional_status)` build a `web.response`; see below |
| `collections` | `range(end)` or `range(start,end)` returns `int[]`; dynamic array/map methods described in [collections](../docs/collections.md) |
| `testing` | `assert(bool, optional_string_message)`, `live_objects() returns int` |

`json.parse` produces a `json.value`, which can be inferred or explicitly named in type annotations (including module aliases). `get` currently selects an object member, not an array index. `kind` returns `null`, `bool`, `number`, `string`, `array`, or `object`. Number tokens preserve their decimal spelling; `as_int` requires an integer in the signed 64-bit range. Parsing handles nested objects/arrays, escaped Unicode and surrogate pairs, and limits nesting to 256. Duplicate keys, malformed data, wrong scalar types, and missing keys are errors. `stringify` emits objects in sorted-key order. Generic Flywheel-value serialization and JSON array access are pending.

Every generated class has `Class.from_json(string)` for scalar string, numeric, and bool fields. Missing defaulted fields use their declaration defaults. Missing required fields and invalid types fail at runtime. Extra JSON fields are currently ignored. Nested class fields, nullable fields, and array fields are not yet supported by this generated conversion.

`web.serve` binds/listens on IPv4 loopback and immediately returns the bound URL, including its route (for example `http://127.0.0.1:8000/`). Port `0` requests an available port; the returned URL contains the actual assigned port. UTF-8 and spaces in route paths are percent-encoded. Startup/bind failures throw `RuntimeError` instead of returning a URL.

```flywheel
var url = web.serve("/", index, 8000)
print "Serving at {url}"
```

The runtime automatically enters its blocking request loop after top-level startup code completes, before releasing global ARC roots. Thus existing standalone `web.serve` statements keep serving, and the URL can be printed or stored first. To begin handling requests before startup finishes, call `web.wait()` explicitly. Handlers execute serially on the same thread; no background thread or parallel mutation is introduced. A try/catch around `serve` handles startup failures; put `web.wait()` inside a try/catch to handle propagated request-handler failures. Unhandled handler errors stop the active listeners and propagate to the application. Calling `web.wait()` recursively from a handler raises `RuntimeError`.

Any number of routes can share a port; routes are matched in registration order. `serve` registers a GET route (which also answers HEAD); `web.route("POST", "/users", create, port)` registers any method, and `"*"` matches every method. A pattern segment `:name` captures one path segment into `req.params["name"]`, and a trailing `*` captures the rest of the path. Unknown paths return 404; a known path with the wrong method returns 405 with an `Allow` header. Port `0` always opens a new listener, so use a fixed port to put several routes on one listener.

A handler takes no arguments or one `web.request`, and returns `element`, `fragment`, `string` (sent as `text/plain`), or `web.response`. Handlers can be named functions or lambdas.

```flywheel
function user(web.request req) returns web.response
    var id = req.params["id"]
    if id in users then
        return web.html(<p>User {users[id]}</p>)
    end
    return web.text("no user " + id, 404)
end
web.serve("/users/:id", user, 8000)
web.route("POST", "/users", function(web.request req) returns web.response
    users[req.form["id"]] = req.form["name"]
    return web.redirect("/users/" + req.form["id"], 303)
end, 8000)
web.static("/assets", "public", 8000)
```

`web.request` has `method`, `path` (percent-decoded), `query`, `headers` (lower-cased names; repeats joined with `, `), `body`, `params`, and `form` (parsed from `application/x-www-form-urlencoded` bodies). Fields are ordinary `string` / `map<string, string>` values. `web.response` has `status` (100..599), `body`, and `headers`; set `content-type` there to override the type. The helpers `web.text`, `web.html` (accepts markup), `web.json` (accepts a string or `json.value`), and `web.redirect` (sets `location`, default status 302) take an optional status and return a response whose fields can still be changed.

`web.static(prefix, directory, port)` serves files below `directory` with a content type chosen by extension and `index.html` for directories. `..` segments, including percent-encoded ones, return 404. A missing directory throws `FileError`.

Requests allow up to 64 KiB of headers and an 8 MiB `Content-Length` body; chunked uploads return 501. Connections close after each response and use socket timeouts. Persistent connections, worker concurrency, TLS, and a general async I/O API are not implemented.

`io` and `net` are not implemented. The remaining APIs in partial modules remain work items. Library failures propagate as typed errors; see [error handling](../docs/errors.md). Uncaught errors report to stderr and exit with status 1. `fs.try_read` separately returns a typed Result for file failures.

See [value conversions](../docs/value-conversions.md) for nested formatting, atomic string-to-sequence casts, repetition, and tuple range operators.
