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
    subprocess.run(
        common + ["-c", str(root / "runtime/runtime.cpp"), "-o", str(d / "runtime.o")],
        check=True,
    )
    for name, (source, expected) in cases.POSITIVE.items():
        (d / "case.fly").write_text(source)
        subprocess.run(
            [
                str(compiler),
                "check",
                str(d / "case.fly"),
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
        result = subprocess.run(
            [str(d / "case")],
            cwd=d,
            check=False,
            capture_output=True,
            text=True,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"},
        )
        if result.returncode or result.stdout != expected:
            print(name, result.returncode, result.stdout, result.stderr, flush=True)
            sys.exit(1)
        print(name, "OK", flush=True)
