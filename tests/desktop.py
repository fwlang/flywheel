"""Real WebKit/GTK window test; run under dbus-run-session and xvfb-run on Linux."""

import ctypes
import ctypes.util
import http.server
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import urllib.parse
from pathlib import Path


def close_window(window):
    """Send WM_DELETE_WINDOW directly: Xvfb has no window manager for EWMH requests."""

    class Data(ctypes.Union):
        _fields_ = [
            ("bytes", ctypes.c_char * 20),
            ("shorts", ctypes.c_short * 10),
            ("longs", ctypes.c_long * 5),
        ]

    class Message(ctypes.Structure):
        _fields_ = [
            ("type", ctypes.c_int),
            ("serial", ctypes.c_ulong),
            ("send_event", ctypes.c_int),
            ("display", ctypes.c_void_p),
            ("window", ctypes.c_ulong),
            ("message_type", ctypes.c_ulong),
            ("format", ctypes.c_int),
            ("data", Data),
        ]

    class Event(ctypes.Union):
        _fields_ = [("message", Message), ("padding", ctypes.c_long * 24)]

    x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XSendEvent.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_int,
        ctypes.c_long,
        ctypes.POINTER(Event),
    ]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    assert display, "Cannot open isolated X display"
    try:
        event = Event()
        event.message.type = 33  # ClientMessage
        event.message.send_event = 1
        event.message.display = display
        event.message.window = int(window)
        event.message.message_type = x11.XInternAtom(display, b"WM_PROTOCOLS", 0)
        event.message.format = 32
        event.message.data.longs[0] = x11.XInternAtom(display, b"WM_DELETE_WINDOW", 0)
        assert x11.XSendEvent(display, int(window), 0, 0, ctypes.byref(event))
    finally:
        x11.XCloseDisplay(display)  # Flushes the request.


def main():
    compiler = Path(sys.argv[1]).resolve()
    received = queue.Queue()
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            requests.append(self.path)
            path = urllib.parse.urlsplit(self.path)
            if path.path == "/probe.js":
                script = (
                    f"fetch('http://127.0.0.1:{self.server.server_port}/result?value=' + "
                    "encodeURIComponent(document.querySelector('h1').textContent + '|' + "
                    "document.querySelector('p').getAttribute('data-note'))).catch(() => {});"
                )
                body = script.encode()
                content_type = "text/javascript; charset=utf-8"
            else:
                if path.path == "/result":
                    received.put(urllib.parse.parse_qs(path.query)["value"][0])
                body = b"ok"
                content_type = "text/plain"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="fw-desktop-test-") as temporary:
            root = Path(temporary)
            title = "FlywheelDesktopTest" + root.name.removeprefix("fw-desktop-test-")
            source = root / "main.fw"
            source.write_text(
                f'''import desktop as gui
import html.*
import testing
var base=testing.live_objects()
function show returns void
    gui.Window window=gui.window("{title}",480,320)
    var heading=<h1>Hello &lt;World&gt; &amp; café</h1>
    var page=<>
        <meta charset="utf-8" />
        {{heading}}
        <p data-note={{"quote \\\" & <"}}>Fragment</p>
        <script src="http://127.0.0.1:{server.server_port}/probe.js"></script>
    </>
    window.load(page)
    heading.text_content="Changed after load"
    print "before first run"
    (window.run)
    print "after first run"
    window.load(<html><body>
        <h1>Second window</h1>
        <p data-note="again">Reload</p>
        <script src="http://127.0.0.1:{server.server_port}/probe.js"></script>
    </body></html>)
    window.run()
    print "after second run"
end
show()
print testing.live_objects()==base
''',
                encoding="utf-8",
            )
            executable = root / "desktop"
            compiled = subprocess.run(
                [str(compiler), "build", str(source), "-O3", "-o", str(executable)],
                check=False,
                capture_output=True,
                text=True,
                timeout=45,
            )
            assert compiled.returncode == 0, compiled.stderr
            process = subprocess.Popen(
                [str(executable)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                env={
                    **os.environ,
                    "GDK_BACKEND": "x11",
                    "WAYLAND_DISPLAY": "",
                    "WEBKIT_DISABLE_DMABUF_RENDERER": "1",
                    "LIBGL_ALWAYS_SOFTWARE": "1",
                },
            )
            try:
                for expected in [
                    'Changed after load|quote " & <',
                    "Second window|again",
                ]:
                    try:
                        actual = received.get(timeout=30)
                    except queue.Empty as error:
                        raise AssertionError(
                            f"WebKit did not report {expected!r}; requests: {requests}"
                        ) from error
                    assert actual == expected, (actual, expected)
                    windows = subprocess.run(
                        [
                            "xdotool",
                            "search",
                            "--onlyvisible",
                            "--name",
                            "^" + re.escape(title) + "$",
                        ],
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=5,
                    ).stdout.splitlines()
                    assert len(windows) == 1, windows
                    geometry = subprocess.run(
                        ["xdotool", "getwindowgeometry", "--shell", windows[0]],
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=5,
                    ).stdout
                    assert "WIDTH=480\n" in geometry and "HEIGHT=320\n" in geometry, (
                        geometry
                    )
                    close_window(windows[0])
                stdout, stderr = process.communicate(timeout=15)
                assert process.returncode == 0, stderr
                assert (
                    stdout
                    == "before first run\nafter first run\nafter second run\ntrue\n"
                ), (stdout, stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                stdout, stderr = process.communicate(timeout=5)
                if process.returncode:
                    print(stdout, stderr, file=sys.stderr)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("desktop WebKit DOM, escaping, live trees, repeated run, and ARC cleanup OK")
    events(compiler)


def events(compiler):
    """Function event handlers, live re-rendering, JS bindings, and two windows at once."""
    with tempfile.TemporaryDirectory(prefix="fw-desktop-events-") as temporary:
        root = Path(temporary)
        source = root / "events.fw"
        source.write_text(
            """import desktop
import html.*
int count = 0
var main_window = desktop.window("Counter", 400, 300)
var side = desktop.window("Side", 300, 200)
function page() returns element
    return <html><head><title>T</title></head><body>
        <p id="out">{count}</p>
        <input id="name" oninput={function(string v) => print("typed " + v)} />
        <button id="add" onclick={function()
            count += 1
            main_window.load(page())
        end}>Add</button>
    </body></html>
end
main_window.load(page())
side.load(<p id="side">Side window</p>)
int reports = 0
var report = function(string text) returns string
    print "report " + text
    reports += 1
    if reports == 2 then
        desktop.quit()
    end
    return "ok"
end
main_window.bind("report", report)
side.bind("report", report)
main_window.eval("var b = document.getElementById('add'); b.click(); b.click(); var n = document.getElementById('name'); n.value = 'hey'; n.dispatchEvent(new Event('input')); setTimeout(function () {{ report(document.getElementById('out').textContent); }}, 500)")
side.eval("setTimeout(function () {{ report(document.getElementById('side').textContent); }}, 300)")
desktop.run([main_window, side])
print "closed", count
""",
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(compiler), "run", str(source)],
            capture_output=True,
            text=True,
            timeout=60,
            env={
                **os.environ,
                "GDK_BACKEND": "x11",
                "WAYLAND_DISPLAY": "",
                "WEBKIT_DISABLE_DMABUF_RENDERER": "1",
                "LIBGL_ALWAYS_SOFTWARE": "1",
            },
        )
        assert result.returncode == 0, result.stderr
        lines = result.stdout.splitlines()
        assert sorted(lines[:3]) == ["report 2", "report Side window", "typed hey"], lines
        assert lines[3:] == ["closed 2"], lines
    print("desktop events, live updates, bindings, and multiple windows OK")
    resources(compiler)


def png(width, height):
    """A minimal opaque RGB PNG."""
    import struct
    import zlib

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    rows = b"".join(b"\0" + b"\x80" * 3 * width for _ in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def resources(compiler):
    """Relative resource URLs resolve against the working directory; void tags need no close."""
    with tempfile.TemporaryDirectory(prefix="fw-desktop-resources-") as temporary:
        root = Path(temporary)
        (root / "pixel.png").write_bytes(png(3, 2))
        source = root / "resources.fw"
        source.write_text(
            """import desktop
import html.*
var window = desktop.window("Resources", 300, 200)
window.load(<html><body><img src="pixel.png" alt="missing"><p id="after">after</p></body></html>)
window.bind("report", function(string text) returns string
    print text
    desktop.quit()
    return ""
end)
window.eval("window.addEventListener('load', function () {{ var i = document.querySelector('img'); report(i.naturalWidth + 'x' + i.naturalHeight + ' ' + i.childNodes.length + ' ' + document.getElementById('after').parentNode.tagName); }})")
window.run()
""",
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(compiler), "run", str(source)],
            capture_output=True,
            text=True,
            timeout=60,
            cwd=root,
            env={
                **os.environ,
                "GDK_BACKEND": "x11",
                "WAYLAND_DISPLAY": "",
                "WEBKIT_DISABLE_DMABUF_RENDERER": "1",
                "LIBGL_ALWAYS_SOFTWARE": "1",
            },
        )
        assert result.returncode == 0, result.stderr
        assert result.stdout == "3x2 0 BODY\n", (result.stdout, result.stderr)
    print("desktop relative resources and void elements OK")


if __name__ == "__main__":
    main()
