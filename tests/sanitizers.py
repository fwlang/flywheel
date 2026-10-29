"""Run positive programs with the native runtime under AddressSanitizer and UBSan (Clang)."""

import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
compiler = pathlib.Path(
    sys.argv[1] if len(sys.argv) > 1 else root / "build/flywheel"
).resolve()
sys.argv = ["tests/test_compiler.py", str(compiler)]
spec = importlib.util.spec_from_file_location("cases", root / "tests/test_compiler.py")
cases = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cases)
with tempfile.TemporaryDirectory(prefix="fw-sanitizers-") as tmp:
    d = pathlib.Path(tmp)
    common = [
        "clang++",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-g",
        "-O1",
        "-std=c++20",
        "-pthread",
    ]
    if sys.platform.startswith("linux"):
        common.append("-ldl")
    subprocess.run(
        common + ["-c", str(root / "runtime/runtime.cpp"), "-o", str(d / "runtime.o")],
        check=True,
    )

    def compile_native(source):
        subprocess.run(
            [
                str(compiler),
                "check",
                str(source),
                "--emit-ir",
                str(d / "case.ll"),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            common + [str(d / "case.ll"), str(d / "runtime.o"), "-o", str(d / "case")],
            check=True,
            capture_output=True,
        )
        return d / "case"

    def verify(name, source, expected, cwd, stdin=None, extra_env=None):
        compile_native(source)
        result = subprocess.run(
            [str(d / "case")],
            cwd=cwd,
            check=False,
            capture_output=True,
            text=True,
            input=stdin,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1", **(extra_env or {})},
        )
        if result.returncode or result.stdout != expected:
            print(name, result.returncode, result.stdout, result.stderr, flush=True)
            sys.exit(1)
        print(name, "OK", flush=True)

    for name, (text, expected) in cases.POSITIVE.items():
        source = d / "case.fly"
        source.write_text(text, encoding="utf-8")
        verify(name, source, expected, d)
    for name, (text, stdin, expected) in cases.INPUT_CASES.items():
        source = d / "case.fly"
        source.write_text(text, encoding="utf-8")
        verify("input_" + name, source, expected, d, stdin)
    source = d / "desktop_case.fly"
    source.write_text(cases.DESKTOP_FAILURE, encoding="utf-8")
    verify(
        "desktop_failure_cleanup",
        source,
        "desktop error\ncleanup\n" * 2 + "true\n",
        d,
        extra_env={"FLYWHEEL_DESKTOP_BACKEND": str(d / "missing-backend")},
    )
    for name, files, expected in cases.MODULE_CASES:
        directory = d / name
        directory.mkdir()
        for filename, text in files.items():
            path = directory / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        verify("module_" + name, directory / "main.fw", expected, directory)

    # Exit through a handler error so ASan/LSan can inspect normal server teardown.
    import http.client
    import queue
    import threading
    import urllib.parse

    source = d / "http.fly"
    source.write_text(
        'import web\nimport html.*\nint calls = 0\nfunction handler returns element\n    calls += 1\n    if calls > 1 then\n        throw RuntimeError("server finished")\n    end\n    return <p>{url}</p>\nend\nfunction start returns string\n    var route = "/owned"\n    return web.serve(route, handler, 0)\nend\nstring url = start()\nprint url\n',
        encoding="utf-8",
    )
    executable = compile_native(source)
    process = subprocess.Popen(
        [str(executable)],
        cwd=d,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"},
    )
    output = queue.Queue()
    reader = threading.Thread(
        target=lambda: output.put(process.stdout.readline().strip()), daemon=True
    )
    reader.start()
    try:
        url = output.get(timeout=10)
        address = urllib.parse.urlsplit(url)
        if address.scheme != "http" or not address.port:
            raise RuntimeError("missing server URL: " + url)
        connection = http.client.HTTPConnection(
            address.hostname, address.port, timeout=3
        )
        connection.request("GET", address.path)
        response = connection.getresponse()
        if response.status != 200 or response.read().decode() != "<p>" + url + "</p>":
            raise RuntimeError("wrong sanitized HTTP response")
        connection.close()
        connection = http.client.HTTPConnection(
            address.hostname, address.port, timeout=3
        )
        connection.request("GET", address.path)
        try:
            connection.getresponse()
        except (http.client.RemoteDisconnected, ConnectionResetError):
            pass
        finally:
            connection.close()
        reader.join(timeout=2)
        _, errors = process.communicate(timeout=10)
        if (
            process.returncode != 1
            or "RuntimeError: server finished" not in errors
            or "Sanitizer" in errors
            or "runtime error:" in errors.replace("Flywheel runtime error:", "")
        ):
            raise RuntimeError(errors)
        print("http_url_ownership OK", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        reader.join(timeout=2)
        process.stdout.close()
        process.stderr.close()
