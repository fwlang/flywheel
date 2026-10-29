# Desktop windows

The native `desktop` library displays Flywheel markup in a window using [webview/webview](https://github.com/webview/webview), backed by GTK/WebKitGTK on Linux and WebView2 on Windows. It does not run a local HTTP server or require Python. Windows can run Flywheel event handlers, expose Flywheel functions to page JavaScript, update live, and run several at once.

```flywheel
import desktop
import html.*

int count = 0
var window = desktop.window("Counter", 480, 320)

function page() returns element
    return <html><body>
        <p id="count">Clicked {count} times</p>
        <button onclick={function()
            count += 1
            window.load(page())
        end}>Click me</button>
    </body></html>
end

window.load(page())
window.run()
```

| API | Behavior |
|---|---|
| `desktop.window()` | Create a window with title `Flywheel`, width `960`, height `640` |
| `desktop.window(string title, int width, int height)` | Customize title and size; trailing arguments are optional; `desktop.window "Title"` also works |
| `window.load(element_or_fragment)` | Show this tree; while running, the page shows it after the current callback |
| `window.bind(string name, function(string) returns string)` | Expose a Flywheel function to page JavaScript as `name(value)` |
| `window.eval(string script)` | Run JavaScript in the page: after loading when called before `run`, otherwise after the current callback |
| `window.run()` | Show the window and block until it closes |
| `desktop.run(desktop.window[] windows)` | Show several windows with one event loop; returns when all are closed |
| `desktop.quit()` | Close every window and return from `run` after the current callback |

`desktop.window` without parentheses is the class itself (`var Window = desktop.window`, then `Window("Title")`); see [classes and constructors](types.md#classes-and-constructors). `desktop.Window` is an alias. Windows are reference-managed objects: assignments alias the same window.

## Events and live updates

Markup event attributes can hold Flywheel functions: `function() returns void`, or `function(string value) returns void`, which receives the event target's `value` (for example the text of an `<input>`). Inline JavaScript strings (`onclick="..."`) still work too.

A window keeps the tree it was given. After every event handler and binding call, each running window re-renders its tree; when the HTML changed, the page body is replaced, keeping the focused element (by `id`) and its text selection. So a handler can mutate the tree in place (`node.text_content = ...`) or call `window.load(new_tree)`. Changes made before `run` are shown when the window opens.

## JavaScript bindings

```flywheel
import desktop
var window = desktop.window()
window.bind("greet", function(string name) returns string
    return "Hello, " + name
end)
window.eval("greet('Ann').then(function (reply) {{ document.title = reply; }})")
```

Page JavaScript calls a binding with one argument and receives a Promise. A single string argument arrives unchanged; other arguments arrive as their JSON text. The returned string resolves the promise. Bindings must be added before `run`. Remember that `{` interpolates in Flywheel strings, so write `{{` and `}}` for JavaScript braces.

Handlers and bindings run on the thread that called `run`, one at a time. An uncaught Flywheel error in a callback closes the windows and propagates from `run` as an ordinary error.

## Errors and lifetime

Dimensions must be positive signed 32-bit values. Titles and rendered HTML cannot contain NUL bytes. Invalid values, a missing backend, a missing display/WebView2 runtime, or native webview failures raise `DesktopError`. Backend modules stay loaded until process exit because GUI frameworks register process-wide types. Relative resource URLs have no application filesystem base; use absolute resource URLs when needed.

## Build and run

```sh
cmake -S . -B build -DFLYWHEEL_DESKTOP=ON
cmake --build build --parallel 2
build/flywheel run examples/desktop_demo.fw
```

The `FLYWHEEL_DESKTOP` CMake setting accepts `AUTO` (default), `ON`, or `OFF`. `AUTO` builds the backend when native development dependencies are available. `ON` makes missing build dependencies a configuration error. `OFF` retains the window API, but `run` raises `DesktopError` unless a compatible backend is provided. Command-line programs do not load the GUI backend.

The build downloads webview **0.12.0** from its official source archive and verifies its SHA-256. For an offline build, provide an existing checkout with `-DFETCHCONTENT_SOURCE_DIR_WEBVIEW=/path/to/webview`. The adapter works around the pinned version's GTK size-setting result bug: GTK applies valid dimensions but reports `Invalid hint`. Only that exact false error is ignored.

- **Linux:** install GTK and WebKitGTK development packages. Debian/Ubuntu: `libgtk-3-dev libwebkit2gtk-4.1-dev`; Arch Linux: `gtk3 webkit2gtk-4.1`. Running a window needs a graphical session.
- **Windows:** use the native C++ toolchain; upstream CMake obtains the WebView2 SDK. End users need the WebView2 runtime installed. This path is untested.

`cmake --install build --prefix /chosen/prefix` installs the backend beside the runtime archive in `lib`. The compiler records its path in generated applications; `FLYWHEEL_DESKTOP_BACKEND` overrides it. The backend exports the versioned entry point `fw_desktop_run_v2`, separate from the core runtime ABI.

## Validation

The ordinary regression suite tests constructors, aliases, ownership, argument diagnostics, and failed-backend cleanup without opening a window. On Linux, CMake also registers a real GUI test when `xvfb-run`, `xdotool`, and `dbus-run-session` are installed (`ctest --test-dir build --output-on-failure`). It reads the rendered DOM, checks escaping and live trees, reruns a window, clicks buttons and types into inputs from JavaScript, checks that Flywheel handlers ran and the page re-rendered, calls bindings in two windows at once, and quits from Flywheel.
