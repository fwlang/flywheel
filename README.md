# Flywheel

A C++20 compiler that turns Flywheel source into LLVM IR, native object files, and executables. Native markup and components become compiled element construction with escaped HTML serialization. Programs require neither Python nor a runtime interpreter.

Flywheel is currently in its **beta** phase: version **2.3 (beta-3)** (`flywheel --version`).

## Build

Requirements: CMake 3.24+, a C++20 toolchain, LLVM development libraries **21–23**, and Python 3 for the development test harness. LLVM 21.1.8 and 23.1.3 have been tested on Linux. A native linker and C++ runtime development files are required to build Flywheel programs. On Linux, `flywheel run` JIT-compiles the program in memory and needs no linker; `flywheel build` links through the configured C++ compiler and uses mold or lld when available (override with `-DFLYWHEEL_LINKER=mold|lld|default`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

On Windows, configure from an x64 Visual Studio developer shell using an LLVM development package built for MSVC; `LLVM_DIR` points to the package directory containing `LLVMConfig.cmake`. CI builds on Ubuntu and Windows with conda-forge's LLVM package. Only Linux has been run in this workspace; macOS is not currently a supported target.

## Use

```sh
build/flywheel run examples/hello.fly
build/flywheel run app.fw -- arguments for the program
build/flywheel build examples/core.fly -O3 -o core
build/flywheel check examples/demo.fly --emit-ir demo.ll
build/flywheel desktop my_app.fw           # show the element the file returns in a window
build/flywheel serve my_app.fw --port 8080 # serve it at http://127.0.0.1:8080/
build/flywheel build -g app.fw -o app      # DWARF lines and locals for gdb/lldb
build/flywheel run --race app.fw           # stop on data races between threads
build/flywheel test --coverage             # run test_*.fw / *_test.fw files, report lines
build/flywheel fmt --check src/*.fw        # indentation formatter
build/flywheel doc lib.fw -o lib.html      # HTML from documentation comments
build/flywheel repl                        # interactive session
build/flywheel lsp                         # language server for editors
```

`run` compiles and executes the program, returning its exit status; a top-level `return 3` ends the program with status 3. `desktop` and `serve` run a program whose top level returns an element or fragment, and show it in a native window or serve it over HTTP ([details](docs/tooling.md#pages-desktop-and-serve)). `check` runs the front end and verifies generated LLVM IR without linking. Optimization levels `-O0` through `-O3` are accepted. `flywheel selftest` runs the compiler's own regression suite. See [tooling](docs/tooling.md).

## A tour

```flywheel
import html.*

interface Shape
    function area() returns double
end

class Circle implements Shape
    double radius
    function area() returns double
        return 3.14159 * inst radius * inst radius
    end
end

class Square implements Shape
    double side
    function init(double side)
        inst side = side
    end
    function area() returns double
        return inst side * inst side
    end
end

enum Size
    Small, Large
end

function size(Shape shape) returns Size
    match shape
        case Circle c if c.radius > 2.0 then
            return Size.Large
        case Square s if s.side > 3.0 then
            return Size.Large
        else
            return Size.Small
    end
end

class Box<T>
    T value
end

Shape[] shapes = [Circle(1.0), Square(4.0)]
for shape in shapes do
    print shape.area(), size(shape)
end

int? best = null
print best ?? 0, "héllo".length, 7.5 // 2, 1 < 2 < 3
print Box("generic").value
print <ul>{shapes.map(function(Shape s) => <li>{s.area()}</li>)}</ul>
```

The language has classes with custom `init`, `private` and `readonly` members, and operator methods; single inheritance, interfaces, enums (including enums with data), exhaustive `match` with destructuring, monomorphized generics with constraints and generic methods, nullable values (including `int?`) with `??` and null-check narrowing, `char`, closures that share captured variables, first-class methods and constructors, sets and custom map keys, frozen collections, `defer`, format specs in interpolation, and an iterator protocol. See [language decisions](docs/language.md), [types](docs/types.md), and [collections](docs/collections.md).

```flywheel
enum Token
    Number(int value)
    Name(string text)
    Plus
end

function describe(Token t) returns string
    match t
        case Token.Number(n) if n < 0 then
            return "negative {n}"
        case Token.Number(n) then
            return "number {n:,}"
        case Token.Name(text) then
            return "name " + text
        case Token.Plus then
            return "plus"
    end
end

var (label, count) = ["tokens", 3]
print describe(Token.Number(1500)), "{label}: {count:>4}"
```

## Constructors

A bare class name is the class itself; calling it constructs:

```flywheel
import desktop
var Window = desktop.window             # the class
var main = desktop.window()             # a window with defaults
var named = desktop.window "my-window"  # outer call without parentheses
var same = desktop.window("my-window")
```

## Libraries

Native modules cover strings, math, files and paths, regular expressions, time and dates, JSON (indexing, encoding any value, typed decoding into nested classes), terminal and file streams, child processes with captured output, timeouts, pipes, environment and working directory, TCP sockets, threads with channels and mutexes, cycle collection, HTTP/HTTPS servers with keep-alive, chunked uploads, and optional handler worker threads, an HTTP/HTTPS client, hashing and HMAC, secure random numbers, base64/hex/URL encodings, UUIDs, gzip/zlib/zip compression, SQLite databases, TOML and YAML parsing, HTML parsing, and desktop windows with event handlers, JavaScript bindings, live updates, and multiple windows. Library modules written in Flywheel add CSV, URLs, logging, command-line arguments, big integers, and exact decimals. See the [library reference](stdlib/README.md), [input and processes](docs/input-processes.md), [concurrency](docs/concurrency.md), and [desktop windows](docs/desktop.md).

```flywheel
import html.*
import web
function index() returns element
    return <html><body><h1>Hello</h1></body></html>
end
print web.serve("/", index, 8000, "0.0.0.0")
```

Markup tags come from `import html.*`, `import svg.*`, or declared [element families](docs/html-dom.md#element-families), and trees support CSS selectors (including pseudo-classes and sibling combinators), parent links, `closest`, and DOM-style mutation. See [HTML tools](docs/html-dom.md).

## Memory

Values are reference counted with atomic counts and freed deterministically; a synchronous cycle collector reclaims unreachable cycles, and `weak<T>` is available for back links. See [runtime ownership](docs/runtime.md).

## Modules and errors

Local `.fw` / `.fly` imports resolve first, then installed packages (`.flywheel-modules`, then the installation's `modules` directory), then library and native modules. Packages are installed by [Pulley](../pulley/README.md) (`pulley` / `ppm`) from Distrobase registries; the `flywheel` command has no package commands. Top-level declarations are public by default; `private` hides implementation names. Dependency initialization runs once, and import cycles and wildcard collisions are diagnosed. See [modules](docs/modules.md). Typed `try`/`catch`/`finally`, `Result<T,E>`, and typed library errors are described in [error handling](docs/errors.md).

## Development and measurements

The black-box regression suite builds independent executables at `-O0` and `-O3`, compares their output, checks compile diagnostics and runtime traps, and exercises live HTTP/HTTPS servers, processes, sockets, threads, the formatter, the test runner, and the language server. For runtime ownership checks with Clang and its sanitizer runtimes installed:

```sh
python3 tests/sanitizers.py build/flywheel
```

[Benchmarks](benchmarks/README.md) compare equivalent observable workloads with C++ and CPython; there is no general speed advantage claim.

An optional local install is supported:

```sh
cmake --install build --prefix "$PWD/install"
install/bin/flywheel run examples/hello.fly
```

The installed compiler locates its runtime archive in `../lib`; the C++ linker toolchain and LLVM shared libraries must remain available.

## VS Code

The [Flywheel VS Code extension](https://github.com/fwlang/vscode-flywheel) supplies syntax highlighting and snippets for `.fw` / `.fly` files, and starts `flywheel lsp` for diagnostics, hover, go to definition, references, rename, completion, document symbols, and formatting (set `flywheel.path` if the compiler is not on `PATH`). Other editors can use `flywheel lsp` through a generic LSP client.
