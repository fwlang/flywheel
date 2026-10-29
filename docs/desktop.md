# Desktop windows

The native `desktop` library displays Flywheel markup in a window using [webview/webview](https://github.com/webview/webview), backed by GTK/WebKitGTK on Linux, Cocoa/WebKit on macOS, and WebView2 on Windows. It does not run a local HTTP server or require Python.

```flywheel
import desktop
import html.*

var window = desktop.window("My application", 960, 640)
window.load(<>
    <h1>Hello, World!</h1>
    <p>Elements and fragments both work.</p>
</>)
window.run()
```

| API | Behavior |
|---|---|
| `desktop.window` or `desktop.window()` on an assignment RHS | Create a window description with title `Flywheel`, width `960`, height `640` |
| `desktop.window(string title, int width, int height)` | Customize title and initial size; trailing arguments are optional |
| `window.load(element_or_fragment) returns void` | Serialize a snapshot of the markup with the existing HTML renderer |
| `window.run() returns void` | Create the native window, display the loaded HTML, and block until it closes |

`desktop.Window` is an alias for `desktop.window` in constructors and type annotations. Module aliases work, for example `import desktop as gui` and `gui.window view = gui.Window("Example")`. Windows are reference-managed objects: assignments alias the same description, and nullable `desktop.window?` values use normal checked access.

A bare class name on the outer right-hand side of a declaration or assignment constructs an instance with no arguments. Parentheses around the class name preserve a constructor reference:

```flywheel
var window = desktop.window       # New window with default title and size
var Window = (desktop.window)     # Constructor reference; creates no window
var custom = Window("Custom", 800, 600)
var another = Window              # Invoke the stored constructor with defaults
var same_constructor = (Window)   # Copy the reference
```

`desktop.Window`, module aliases, and user-defined classes follow the same rule. Ordinary instance assignment, such as `var alias = window`, still shares the existing instance. `(desktop.window)()` explicitly invokes the constructor; `(desktop.window())` groups an already-created instance. Constructor references can be stored in inferred bindings and called, but have no instance fields/methods and cannot currently be stored in collections or given a callable parameter type. Grouped standalone constructor references are not invoked; ordinary zero-argument function and method statements still invoke, including `(window.run)`. Function references keep their existing semantics.

Construction and `load` do not open a window or require a graphical session. `load` copies the serialized HTML, so source elements can be mutated or released afterward without changing the displayed snapshot. It accepts both individual elements and fragments; strings are rejected. It preserves existing text/attribute escaping. A new `load` replaces the previous snapshot. Without `load`, `run` shows an empty document.

Closing the native window returns from `run` and releases its native resources. Calling `run` again creates another window from the saved snapshot; calling `load` between runs replaces its contents. Execution stays on the calling/main thread. `run` is blocking, so this API does not supply parallel windows, live synchronization with Flywheel trees, native event callbacks, or a JavaScript-to-Flywheel bridge. Existing server-side markup restrictions on event attributes still apply. Relative resource URLs have no application filesystem base; use absolute resource URLs when needed.

Dimensions must be positive signed 32-bit values. Titles and rendered HTML cannot contain NUL bytes. Invalid values, a missing backend, a missing display/WebView2 runtime, or native webview failures raise `DesktopError`. Ordinary `try`/`catch`/`finally` and ARC cleanup apply. Backend modules stay loaded until process exit because GUI frameworks register process-wide types.

## Build and run

```sh
cmake -S . -B build -DFLYWHEEL_DESKTOP=ON
cmake --build build --parallel 2
build/flywheel run examples/desktop_demo.fw
```

The `FLYWHEEL_DESKTOP` CMake setting accepts `AUTO` (default), `ON`, or `OFF`. `AUTO` builds the backend when native development dependencies are available. `ON` makes missing build dependencies a configuration error. `OFF` retains the window value and serialization API, but `run` raises `DesktopError` unless a compatible backend is provided. Command-line programs do not load the GUI backend.

The build downloads webview **0.12.0** from its official source archive and verifies its SHA-256. For an offline build, provide an existing checkout with `-DFETCHCONTENT_SOURCE_DIR_WEBVIEW=/path/to/webview`. Webview's own examples, tests, and unrelated libraries are disabled. The adapter works around the pinned version's GTK size-setting result bug: GTK applies valid dimensions but reports `Invalid hint`. Only that exact false error is ignored; the GUI test checks the actual window dimensions.

- **Linux:** install GTK and WebKitGTK development packages. Debian/Ubuntu: `libgtk-3-dev libwebkit2gtk-4.1-dev`; Arch Linux: `gtk3 webkit2gtk-4.1`. WebKitGTK 6.0/GTK4 and 4.0/GTK3 are also supported by the upstream backend. Running a window needs a graphical session and the corresponding runtime libraries.
- **macOS:** use the native development toolchain and system Cocoa/WebKit frameworks. GUI execution must occur on the main thread.
- **Windows:** use the native C++ toolchain; upstream CMake obtains the WebView2 SDK. End users need the WebView2 runtime installed.

`cmake --install build --prefix /chosen/prefix` installs the backend beside the runtime archive in `lib`. The compiler locates that backend and records its path in generated applications. For deployment elsewhere, distribute the matching `libflywheel_desktop.so`, `libflywheel_desktop.dylib`, or `flywheel_desktop.dll`, install its platform runtime dependencies, and set `FLYWHEEL_DESKTOP_BACKEND` to its path. This environment variable overrides the recorded path. The shared backend exports a versioned native entry point, `fw_desktop_show_v1`; it is separate from the core runtime ABI.

## Validation

The ordinary regression suite tests constructors, aliases/type annotations, markup ownership, replacing content, argument diagnostics, and failed-backend cleanup without opening a window. ASan/UBSan covers these ownership/error paths.

On Linux, CMake also registers a real GUI test when `xvfb-run`, `xdotool`, and `dbus-run-session` are installed:

```sh
ctest --test-dir build --output-on-failure
```

The isolated WebKit test reads the actual rendered DOM through a loopback test script, checks escaping and snapshot behavior, closes the window gracefully, runs it a second time with new content, and checks Flywheel object cleanup. Linux is the tested platform at this checkpoint; Windows/macOS backend execution has not been verified locally.
