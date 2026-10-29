# Flywheel

A C++20 compiler that turns Flywheel source into LLVM IR, native object files, and executables. Native markup and components become compiled element construction, with escaped HTML serialization. Programs require neither Python nor a runtime source/AST interpreter.

This is a working **partial implementation** of the Flywheel language. The full language and standard library remain the target.

## Build

Requirements: CMake 3.24+, a C++20 toolchain, LLVM development libraries **21–23**, and Python 3 for the development test harness. LLVM 21.1.8 and 23.1.3 have been tested on Linux. LLVM 22 is accepted but has not been tested locally. Native linker and C++ runtime development files are required to build Flywheel programs. On Linux, `flywheel run` JIT-compiles the program in memory and needs no linker; `flywheel build` links through the configured C++ compiler and uses mold or lld when available (override with `-DFLYWHEEL_LINKER=mold|lld|default`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

On Windows, configure from an x64 Visual Studio developer shell using an LLVM development package built for MSVC. On macOS, use the native Apple toolchain and an LLVM development package for the machine architecture. `LLVM_DIR` points to the package directory containing `LLVMConfig.cmake`. The CI workflow uses conda-forge's LLVM development package on all three OSes. Windows and macOS runs have **not** been executed in this workspace.

## Use

```sh
build/flywheel --version
build/flywheel check examples/demo.fly
build/flywheel run examples/hello.fly
build/flywheel build examples/core.fly -O3 -o core
./core
build/flywheel check examples/components.fly --emit-ir components.ll
build/flywheel test
```

`run` compiles and executes a temporary native binary, returning its exit status. `check` runs the frontend and verifies generated LLVM IR without linking. `--emit-ir` writes verified IR; optimization levels `-O0` through `-O3` are accepted. `test` runs this repository's development tests, not Flywheel test discovery. Output names default to the source stem, with `.exe` on Windows.

Run the exact canonical example with:

```sh
build/flywheel run examples/demo.fly -O3
```

Open `http://127.0.0.1:8000/`; stop the server with Ctrl+C. `web.serve` returns the URL immediately, so `print web.serve("/", index, 8000)` prints the actual address. Request handling starts automatically after startup code finishes; `web.wait()` can enter the request loop explicitly. Optionally place `user.json` in the working directory containing `{"name":"Danny","age":15}`. Both the missing-file and JSON-file branches are covered by HTTP integration tests. `Person.from_json` is generated for scalar fields, including declared field defaults. It is not a general serialization system yet.

A server can register many routes per port, including `:param` segments and other methods through `web.route`. Handlers may take a `web.request` (path, query, headers, body, form, params) and return a `web.response` built with `web.text`, `web.html`, `web.json`, or `web.redirect` to set status codes and headers. `web.static` serves a directory of files. See the [web API](stdlib/README.md).

Functions are values: `function(int) returns int` is a type, lambdas capture their surroundings (`function(int x) => x + base`), and arrays support `map`/`filter`/`reduce`. Maps are hashed, and `weak<T>` breaks reference cycles such as parent links. See [language decisions](docs/language.md) and [runtime ownership](docs/runtime.md).

## Examples

```flywheel
import html.*

class Person
    string name
    int age = 0
end

component Card = <div style={attr style}>
    <h1>{attr user.name}</h1>
    <p>Age: {attr user.age}</p>
    {attr inner}
</div>

var user = Person "Danny & friends", 15
print html.render(<Card user={user}>Hello!</Card>)
```

See [language decisions](docs/language.md), [runtime ownership](docs/runtime.md), and the [native library API](stdlib/README.md). Unknown or unsupported functionality produces a diagnostic rather than a success stub. Library I/O failures raise typed errors and can be handled with `try`/`catch`/`finally`; uncaught errors exit with status 1.

## Modules, collections, and errors

Local `.fw` / `.fly` imports resolve before native modules. Top-level declarations are public by default; `private` hides implementation names. Dependency initialization runs once, and import cycles and wildcard collisions are diagnosed. See [modules and visibility](docs/modules.md).

Arrays support `push`, `pop`, and `clear`; maps support typed keys/values and mutation; tuples have fixed typed positions. See [collections](docs/collections.md). Typed `try`/`catch`/`finally`, thrown values, `Result<T,E>`, and `fs.try_read` are documented in [error handling](docs/errors.md).

```sh
build/flywheel run examples/modules/main.fw
build/flywheel run examples/collection_demo.fw
build/flywheel run examples/errors.fw
```

A fragment (`<>...</>`) is a sequence of markup nodes. An `element` binding can retain that sequence without introducing a wrapper tag, and both fragments and named elements support `.render()`:

```flywheel
import html.*
element page = <><h1>Hello</h1><p>Welcome!</p></>
print page.render()
```

## Value and HTML tools

`string`, printing, and interpolation support nested collections, classes, Results, JSON, and markup. Explicit `array<T>`, `tuple<T>`, and `map<K,V>` casts convert nested values; `array("hello")` is `["hello"]`. Strings/arrays/tuples support repetition, and `1..3` / `1..<3` produce homogeneous tuples. See [value conversions](docs/value-conversions.md).

HTML values support explicit-tree selectors and DOM-like tools:

```flywheel
var title = page.query("#main-title")
if title != null then
    title.text_content = "Updated & escaped"
end
array<element> cards = html.query_all(page, ".card p")
```

See [HTML querying and mutation](docs/html-dom.md) for selector syntax, ownership, methods, and limitations. Runnable examples are `examples/value_tools.fw` and `examples/dom_tools.fw`.

## Development and measurements

The black-box regression suite builds independent executables at `-O0` and `-O3`, compares their output, checks compile diagnostics and runtime traps, and exercises a live HTTP server. For runtime ownership checks with Clang and its sanitizer runtimes installed:

```sh
python3 tests/sanitizers.py build/flywheel
```

[Benchmarks](benchmarks/README.md) compare equivalent observable workloads with C++ and CPython, record compilation separately, and retain individual execution samples and peak RSS. They show substantial remaining optimization work; there is no general speed advantage claim.

An optional local install is supported:

```sh
cmake --install build --prefix "$PWD/install"
install/bin/flywheel run examples/hello.fly
```

The installed compiler locates its runtime archive in `../lib`; the C++ linker toolchain and LLVM shared libraries must remain available. Packaging a fully relocatable SDK and installed test discovery are future work.

Read a line with `input("Name: ")`; run child processes with `os.shell("echo hello")` or `os.command(["git", "status", "--short"])`. Both process functions return an integer exit status and inherit standard streams. See [input and processes](docs/input-processes.md).

## VS Code

The [Flywheel extension](fw-vscode/README.md) supplies syntax highlighting, snippets, and editing support for `.fw` / `.fly` files. Build and install it locally from `fw-vscode`.

## Desktop windows

`import desktop` provides `desktop.window("Title", 960, 640)`, `.load(element_or_fragment)`, and blocking `.run()` using a native webview. See [desktop setup and APIs](docs/desktop.md) and [the runnable example](examples/desktop_demo.fw).
