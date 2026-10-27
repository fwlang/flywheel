# Flywheel

A C++20 compiler that turns Flywheel source into LLVM IR, native object files, and executables. Native markup and components become compiled element construction, with escaped HTML serialization. Programs require neither Python nor a runtime source/AST interpreter.

This is a working **partial implementation** of the language described in `FLYWHEEL.md`. The full language and standard library remain the target. See [STATUS.md](STATUS.md) for supported features, omissions, and next steps.

## Build

Requirements: CMake 3.24+, a C++20 toolchain, LLVM development libraries **21–23**, and Python 3 for the development test harness. LLVM 21.1.8 and 23.1.3 have been tested on Linux. LLVM 22 is accepted but has not been tested locally. Native linker and C++ runtime development files are required to build Flywheel programs.

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

Open `http://127.0.0.1:8000/`; stop the blocking server with Ctrl+C. Optionally place `user.json` in the working directory containing `{"name":"Danny","age":15}`. Both the missing-file and JSON-file branches are covered by HTTP integration tests. `Person.from_json` is generated for scalar fields, including declared field defaults. It is not a general serialization system yet.

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

See [language decisions](docs/language.md), [runtime ownership](docs/runtime.md), and the [native library API](stdlib/README.md). Unknown or unsupported functionality produces a diagnostic rather than a success stub. Current library I/O failures terminate the program; typed exceptions are still missing.

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
