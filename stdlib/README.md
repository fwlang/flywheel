# Native library reference

Libraries are compiler-resolved declarations backed by C++ runtime functions; no library function uses an interpreter. Library failures raise typed errors (see [error handling](../docs/errors.md)). Import a module with `import name` (or `import name as alias`) and call `name.function(...)`.

| Module | Contents |
|---|---|
| core (implicit) | `print`, `input`, `typeof`, `len`, casts, `string(value)`, strings, collections, `Result` |
| `collections` | `range` |
| `desktop` | native windows with events and JavaScript bindings — [desktop](../docs/desktop.md) |
| `fs` | files, directories, and paths |
| `gc` | cycle collection — [runtime](../docs/runtime.md#cycle-collection) |
| `html`, `svg` | tag sets (`import html.*`, `import svg.*`), rendering, tree queries — [HTML tools](../docs/html-dom.md) |
| `io` | stream-style terminal I/O and files — [input and processes](../docs/input-processes.md) |
| `json` | parsing, querying, encoding, typed decoding |
| `math` | numeric functions and random numbers |
| `net` | TCP sockets and name resolution |
| `os` | environment, arguments, child processes — [input and processes](../docs/input-processes.md) |
| `regex` | regular expressions |
| `testing` | assertions and lifetime metrics — [tooling](../docs/tooling.md) |
| `thread` | tasks, channels, mutexes — [concurrency](../docs/concurrency.md) |
| `time` | clocks, sleeping, calendar dates, formatting |
| `web` | HTTP and HTTPS servers |

## Strings

Strings are immutable UTF-8; lengths and indices count characters. Negative slice indices count from the end, and slice bounds are clamped.

| Member | Result |
|---|---|
| `.length`, `len(s)` | number of characters |
| `s[i]` | `char` at index `i` (`IndexError` when out of range) |
| `s[a..b]`, `s[a..<b]`, `.slice(start)`, `.slice(start, end)` | substring |
| `.upper()`, `.lower()` | case mapping (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic; one character to one character) |
| `.trim()`, `.trim_start()`, `.trim_end()` | remove Unicode whitespace |
| `.split()` | split on runs of whitespace, dropping empty pieces |
| `.split(separator)` | split on an exact string, keeping empty pieces |
| `.lines()` | split on LF / CRLF / CR |
| `.chars()` | `char[]` |
| `.find(sub)`, `.find_last(sub)` | character index or -1 |
| `.contains(sub)`, `.starts_with(sub)`, `.ends_with(sub)`, `.count(sub)` | searching (the argument may be a `char`) |
| `.replace(old, new)` | replace every occurrence |
| `.pad_start(width, fill = " ")`, `.pad_end(width, fill = " ")` | pad to a width |
| `.reverse()`, `.repeat(n)`, `.is_empty()` | |
| `separator.join(sequence)`, `sequence.join(separator)` | concatenate items |
| `sub in s`, `'c' in s` | containment |

`int("42")`, `double("1.5")`, `bool("true")`, and `char("x")` parse strings; `string(value)` formats any value.

## math

Constants `PI`, `TAU`, `E`, `INF`, `NAN`. Functions taking and returning `double`: `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `hypot(x, y)`, `exp`, `log(x)`, `log(x, base)`, `log10`, `log2`, `pow(a, b)`, `floor`, `ceil`, `round` (half away from zero), `trunc`. `is_nan(x)` and `is_finite(x)` return bool.

`abs(x)`, `min(a, b, ...)`, `max(a, b, ...)`, and `clamp(x, low, high)` keep the type of their numeric arguments (`math.abs(-5)` is an `int`; `abs` of the minimum integer raises `NumericError`). `random()` returns a double in [0, 1), `random_int(low, high)` an inclusive integer, and `seed(n)` makes the sequence repeatable. The generator is per thread.

## fs

| Function | Behavior |
|---|---|
| `exists(path)`, `is_dir(path)`, `is_file(path)` | bool |
| `read(path)`, `write(path, text)`, `append(path, text)` | whole-file text |
| `try_read(path)` | `Result<string, FileError>` |
| `size(path)` | bytes |
| `list(path)` | sorted names in a directory |
| `walk(path)` | sorted relative paths of everything below a directory |
| `mkdir(path)` | create a directory and any missing parents |
| `copy(from, to)`, `move(from, to)` | copy (recursively, overwriting) or rename |
| `remove(path)`, `remove_all(path)` | remove a file or empty directory; remove a tree and return the count |
| `join(a, b, ...)` / `join(string[])` | join path parts |
| `basename`, `dirname`, `extension`, `stem`, `absolute`, `normalize` | path text |
| `cwd()`, `temp_dir()` | current and temporary directories |

Paths are UTF-8 strings and are returned with `/` separators. Failures raise `FileError`.

## time

| Function | Behavior |
|---|---|
| `monotonic()` | seconds from a steady clock, for measuring durations |
| `sleep(seconds)` | pause the calling thread |
| `now()` | wall-clock seconds since the Unix epoch |
| `format(seconds, pattern = "%Y-%m-%dT%H:%M:%SZ")` | UTC text using `strftime` patterns |
| `format_local(seconds, pattern = "%Y-%m-%d %H:%M:%S")` | local-time text |
| `parse(text, pattern = "%Y-%m-%dT%H:%M:%SZ")` | seconds, reading the text as UTC |
| `date(seconds)`, `local_date(seconds)` | a `time.datetime` with `year`, `month`, `day`, `hour`, `minute`, `second`, `weekday` (0 = Sunday), `yearday` |
| `from_date(year, month, day, hour = 0, minute = 0, second = 0.0)` | seconds for a UTC calendar time |

## regex

ECMAScript regular expressions (the syntax of JavaScript), applied to the characters of the text, as strings are: `.` and character classes such as `[^a]` match whole characters, so `regex.find(".", "é")` is `"é"`. Shorthand classes (`\w`, `\d`, `\s`, `\b`) and case-insensitive matching cover ASCII only. On Windows, characters outside the Basic Multilingual Plane (such as most emoji) are matched as two units. Patterns are compiled once and cached per thread. Invalid patterns raise `RegexError`.

| Function | Result |
|---|---|
| `matches(pattern, text)` | the whole text matches |
| `contains(pattern, text)` | some part matches |
| `find(pattern, text)` | `string?`: the first match |
| `find_all(pattern, text)` | every match |
| `groups(pattern, text)` | `string[]?`: the capture groups of the first match |
| `replace(pattern, text, replacement)` | replace every match; `$1` refers to groups |
| `split(pattern, text)` | split around matches |

## json

`json.parse(text)` returns a `json.value`. `value["key"]` reads an object field (`JsonError` when missing) and `value[0]` an array element. `json.get(value, key)`, `json.at(value, index)`, `json.has(value, key)`, `json.length(value)`, `json.keys(value)`, `json.items(value)` (`json.value[]`), `json.is_null(value)`, and `json.kind(value)` (`null`, `bool`, `number`, `string`, `array`, `object`) inspect documents; `json.as_string`, `as_int`, `as_double`, `as_bool` convert scalars. `json.stringify(value)` renders a `json.value` with sorted keys.

`json.encode(value, indent = 0)` encodes any value: numbers, bools, strings, chars and enum names as strings, null, optional values, arrays, tuples, and sets as arrays, maps as objects (non-string keys are stringified), class instances as objects in field order, `Result` as `{"ok": ...}` or `{"error": ...}`, errors as `{"type", "message"}`, `json.value` as itself, and markup as its HTML string. Functions, weak references, resources, and cycles raise `JsonError`. `indent > 0` pretty-prints.

`json.decode(text)` converts JSON into the type the result is assigned to; `json.decode<T>(text)` names it explicitly. The argument may be a string or a `json.value`. `Class.from_json(text)` is the same for a class. Decoding supports every scalar, `char`, enums (by name), optional types (`null` becomes null), arrays, `tuple<T>`, fixed tuples, sets, maps with string, integer, bool, char, or enum keys, nested and generic classes, and `json.value`. Class fields are read by name; missing fields use their declared default, or null for optional fields, and are otherwise an error. Extra fields are ignored. Decoding builds instances field by field without running `init`.

```flywheel
import json
class Address
    string city
    int? zip = null
end
class Person
    string name
    int age = 0
    Address[] homes = []
end
Person p = json.decode("{{\"name\": \"Ann\", \"homes\": [{{\"city\": \"Oslo\"}}]}}")
print json.encode(p)
```

## net

| Function or method | Behavior |
|---|---|
| `net.resolve(host)` | the host's IPv4/IPv6 addresses |
| `net.connect(host, port, timeout = 0.0)` | a connected `net.socket` (trying each address) |
| `net.listen(host, port)` | a `net.listener`; port 0 picks a free port; `"::"` and `"0.0.0.0"` listen on all interfaces |
| `listener.accept()` | wait for a client; returns a `net.socket` |
| `listener.port()`, `socket.port()` | local port |
| `socket.send(text)` | send all bytes |
| `socket.receive(limit = 65536)` | up to `limit` bytes; `""` at end of stream |
| `socket.receive_line()` | `string?`: the next line without its line ending; null at end of stream |
| `socket.set_timeout(seconds)` | receive/send timeout |
| `address()`, `peer()` | `"host:port"` of either end (IPv6 hosts in brackets) |
| `close()` | close the socket; sockets also close when released |

Strings carry arbitrary bytes, so binary protocols work, but `length` counts UTF-8 characters. Failures raise `NetworkError`. Sockets block; combine them with [threads](../docs/concurrency.md) to serve several clients.

## os

`env(name)` reads an environment variable (empty when unset), `args()` returns program arguments, and `exit(code)` ends the program. `run`, `process`, `pipeline`, `shell`, and `command` are described in [input and processes](../docs/input-processes.md).

## testing

`assert(bool, message = "")`, `assert_equal(actual, expected, message = "")`, `fail(message)`, and `live_objects()`; see [tooling](../docs/tooling.md).

## web

`web.serve(route, handler, port, host = "127.0.0.1")` registers a GET route (also answering HEAD) and returns the listener URL including the route. `web.route(method, route, handler, port, host)` registers any method (`"*"` matches all). `web.static(prefix, directory, port, host)` serves files. Port `0` opens a new listener on a free port; use a fixed port to put several routes on one listener. `host` may be an IPv4 or IPv6 address or a host name: `"0.0.0.0"` or `"::"` listen on all interfaces.

```flywheel
import html.*
import web
map<string, string> users = {"1": "Ann"}
function user(web.request req) returns web.response
    var id = req.params["id"]
    if id in users then
        return web.html(<p>User {users[id]}</p>)
    end
    return web.text("no user " + id, 404)
end
web.serve("/users/:id", user, 8000, "0.0.0.0")
```

A handler takes no arguments or one `web.request`, and returns `element`, `fragment`, `string` (sent as `text/plain`), or `web.response`. Handlers can be named functions or lambdas. A pattern segment `:name` captures one path segment into `req.params["name"]`, and a trailing `*` captures the rest of the path. Unknown paths return 404; a known path with the wrong method returns 405 with an `Allow` header.

`web.request` has `method`, `path` (percent-decoded), `query`, `headers` (lower-cased names; repeats joined with `, `), `body`, `params`, and `form` (parsed from `application/x-www-form-urlencoded` bodies). `web.response` has `status` (100..599), `body`, and `headers`. `web.text`, `web.html` (accepts markup), `web.json` (a string or `json.value`), and `web.redirect` (sets `location`, default status 302) build responses with an optional status.

`web.tls(certificate_file, key_file, port, host)` serves that port over HTTPS using PEM files and returns its `https://` URL; register routes on the same port afterwards. TLS uses the system OpenSSL 3 library (`libssl`), loaded when `web.tls` is first called, so programs that do not use TLS do not depend on it.

The runtime enters its request loop after top-level startup code completes, or explicitly with `web.wait()`. One thread multiplexes every listener and connection with non-blocking sockets, so slow or idle clients do not hold up others. HTTP/1.1 connections stay open for further requests (`Connection: keep-alive`) until the client closes them, asks to close, or stays idle for 15 seconds. Request bodies may use `Content-Length` or `Transfer-Encoding: chunked`. Headers are limited to 64 KiB and bodies to 8 MiB.

Handlers run one at a time on the serving thread by default. `web.workers(n)` (called before serving starts) runs them on `n` threads; see [concurrency](../docs/concurrency.md#web-handlers). An uncaught handler error stops the listeners and propagates from the request loop; wrap `web.wait()` in try/catch to handle it.

## collections, desktop, html

`collections.range(end)` and `range(start, end)` return `int[]`. The desktop and HTML APIs are documented in [desktop windows](../docs/desktop.md) and [HTML tools](../docs/html-dom.md); collection methods in [collections](../docs/collections.md).
